// Tests for suture failure (threads snapping at their knot-pull strength) and
// for taking a suture out and tying it again exactly where it was (undo).

#include <doctest/doctest.h>

#include <cmath>
#include <map>

#include "skinsim/sheet.hpp"

using namespace skinsim;

namespace {

using V = Eigen::Vector2d;

const SheetGeometry kGeometry{100.0, 60.0, 2.5};

void settle(Simulation& sim) {
    for (int frame = 0; frame < 200; ++frame)
        if (sim.step(20).converged) return;
}

MaterialPoint at(const Simulation& sim, const V& p) {
    MaterialPoint m;
    REQUIRE(sim.material_point_at(p, m));
    return m;
}

// A vertical incision whose right lip is pulled 7 mm away by forceps and
// held there: closing it takes a few newtons. max_force caps the forceps'
// pull (5 N by default).
Sheet held_open_wound(double max_force = GrabParams{}.max_force) {
    Sheet s(kGeometry);
    REQUIRE(s.cut_rest({V(0, -15), V(0, 15)}) > 0);
    Simulation& sim = s.simulation();
    GrabParams g = sim.grab_params();
    g.max_force = max_force;
    sim.set_grab_params(g);
    REQUIRE(sim.grab(0, V(4, 0)));
    sim.move_grab(0, V(11, 0));
    settle(sim);
    return s;
}

// A suture across the held-open wound at height y: 3 mm bites either side.
int suture_across(Simulation& sim, double y, int thread, double tightness = 0.0) {
    MaterialPoint lip;
    REQUIRE(sim.nearest_lip_point(V(0, y), 8.0, lip));
    const V left = sim.position_of(lip) - V(3, 0);  // the left lip barely moved
    MaterialPoint right_lip;
    REQUIRE(sim.opposite_lip_point(lip, right_lip));
    const V right = sim.position_of(right_lip) + V(3, 0);
    return sim.add_suture(at(sim, left), at(sim, right), tightness, thread);
}

}  // namespace

TEST_CASE("thread strength: finer threads are weaker; unknown sizes never break") {
    double previous = 0.0;
    for (int size : {7, 6, 5, 4, 3, 2}) {
        CAPTURE(size);
        CHECK(thread_strength(size) > previous);
        previous = thread_strength(size);
    }
    CHECK(thread_strength(5) == doctest::Approx(3.92));  // USP 5-0, Class I
    CHECK(thread_strength(0) == 0.0);
    CHECK(thread_strength(1) == 0.0);
    CHECK(thread_strength(11) == 0.0);
}

TEST_CASE("suture info: thread tension is half the closing force; load is tension / strength") {
    Sheet s = held_open_wound();
    Simulation& sim = s.simulation();
    REQUIRE(suture_across(sim, 0.0, 5) > 0);
    REQUIRE(suture_across(sim, 8.0, 0) > 0);  // ideal thread
    settle(sim);
    const auto infos = sim.suture_info();
    REQUIRE(infos.size() == 2);
    for (const SutureInfo& su : infos) {
        CHECK(su.force > 0.1);
        CHECK(su.tension == doctest::Approx(0.5 * su.force));
    }
    CHECK(infos[0].thread == 5);
    CHECK(infos[0].strength == doctest::Approx(3.92));
    CHECK(infos[0].load == doctest::Approx(infos[0].tension / 3.92));
    CHECK(infos[1].thread == 0);
    CHECK(infos[1].strength == 0.0);
    CHECK(infos[1].load == 0.0);
}

TEST_CASE("an overloaded thread snaps at equilibrium, only when snapping is on") {
    Sheet s = held_open_wound();
    Simulation& sim = s.simulation();
    const int id = suture_across(sim, 0.0, 7);  // 7-0: snaps at 1.08 N
    REQUIRE(id > 0);
    settle(sim);
    // Snapping is off by default: the suture holds, overloaded.
    REQUIRE(sim.suture_count() == 1);
    const SutureInfo held = sim.suture_info().at(0);
    MESSAGE("7-0 across a held-open wound: thread tension " << held.tension << " N, load " << held.load * 100 << "%");
    CHECK(held.load > 1.0);
    CHECK(sim.take_suture_breaks().empty());

    SutureParams p = sim.suture_params();
    p.can_break = true;
    sim.set_suture_params(p);
    settle(sim);
    CHECK(sim.suture_count() == 0);
    const auto breaks = sim.take_suture_breaks();
    REQUIRE(breaks.size() == 1);
    CHECK(breaks[0].id == id);
    CHECK(breaks[0].thread == 7);
    CHECK(breaks[0].tension > thread_strength(7));
    CHECK(sim.take_suture_breaks().empty());  // reported once
}

TEST_CASE("a strong enough thread holds when snapping is on") {
    Sheet s = held_open_wound();
    Simulation& sim = s.simulation();
    SutureParams p = sim.suture_params();
    p.can_break = true;
    sim.set_suture_params(p);
    REQUIRE(suture_across(sim, 0.0, 2) > 0);  // 2-0: 14.1 N
    settle(sim);
    CHECK(sim.suture_count() == 1);
    CHECK(sim.take_suture_breaks().empty());
    CHECK(sim.suture_info().at(0).load < 1.0);
}

TEST_CASE("an overloaded closure fails one suture at a time, the most loaded first") {
    Sheet s = held_open_wound(20.0);  // a hard pull
    Simulation& sim = s.simulation();
    for (double y : {-6.0, 0.0, 6.0}) REQUIRE(suture_across(sim, y, 7) > 0);
    settle(sim);
    int worst = -1;
    double worst_load = 0.0;
    std::map<int, double> load_before;
    for (const SutureInfo& su : sim.suture_info()) {
        load_before[su.id] = su.load;
        if (su.load > worst_load) {
            worst_load = su.load;
            worst = su.id;
        }
    }
    MESSAGE("worst suture at " << worst_load * 100 << "% of its strength");
    REQUIRE(worst_load > 1.0);

    SutureParams p = sim.suture_params();
    p.can_break = true;
    sim.set_suture_params(p);
    sim.step(20);  // at equilibrium already: checks, and snaps one
    const auto first = sim.take_suture_breaks();
    REQUIRE(first.size() == 1);
    CHECK(first[0].id == worst);
    CHECK(sim.suture_count() == 2);

    // The others take up its share of the load; any that are now overloaded
    // snap in turn, until every remaining suture holds.
    settle(sim);
    const auto later = sim.take_suture_breaks();
    for (const SutureInfo& su : sim.suture_info()) {
        CAPTURE(su.id);
        CHECK(su.load > load_before[su.id]);
        CHECK(su.load <= 1.0);
    }
    MESSAGE(later.size() << " more snapped, " << sim.suture_count() << " hold");
}

TEST_CASE("weakening a suture's thread can make it snap") {
    Sheet s = held_open_wound();
    Simulation& sim = s.simulation();
    SutureParams p = sim.suture_params();
    p.can_break = true;
    sim.set_suture_params(p);
    const int id = suture_across(sim, 0.0, 2);
    REQUIRE(id > 0);
    settle(sim);
    REQUIRE(sim.suture_count() == 1);
    REQUIRE(sim.set_suture_thread(id, 7));
    CHECK(sim.suture_info().at(0).thread == 7);
    settle(sim);
    CHECK(sim.suture_count() == 0);
    CHECK(sim.take_suture_breaks().size() == 1);
    CHECK_FALSE(sim.set_suture_thread(id, 5));  // gone
}

TEST_CASE("a cut suture can be tied again exactly where it was") {
    Sheet s = held_open_wound();
    Simulation& sim = s.simulation();
    const int id = suture_across(sim, 0.0, 4, 0.1);
    REQUIRE(id > 0);
    settle(sim);
    const SutureInfo before = sim.suture_info().at(0);

    SutureRecord record;
    REQUIRE(sim.suture_record(id, record));
    SutureRecord none;
    CHECK_FALSE(sim.suture_record(id + 100, none));
    REQUIRE(sim.remove_suture(id));
    settle(sim);  // the wound springs open again
    REQUIRE(sim.restore_suture(record) == id);  // same id
    settle(sim);
    const SutureInfo after = sim.suture_info().at(0);
    CHECK(after.thread == 4);
    CHECK(after.tightness == doctest::Approx(0.1));
    CHECK(after.force == doctest::Approx(before.force).epsilon(1e-3));
    CHECK((after.a - before.a).norm() < 1e-3);
    CHECK((after.b - before.b).norm() < 1e-3);

    // A record restores even on another mesh of the same skin (after a
    // further incision elsewhere), on the correct lips.
    REQUIRE(sim.suture_record(id, record));
    REQUIRE(sim.remove_suture(id));
    REQUIRE(s.cut_rest({V(-30, -20), V(-20, -20)}) > 0);
    Simulation& remeshed = s.simulation();
    REQUIRE(remeshed.suture_count() == 0);
    REQUIRE(remeshed.restore_suture(record) == id);
    settle(remeshed);
    const SutureInfo moved = remeshed.suture_info().at(0);
    MESSAGE("closing force " << before.force << " N before, " << moved.force << " N after re-meshing");
    CHECK(moved.force == doctest::Approx(before.force).epsilon(0.05));
}
