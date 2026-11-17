// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "core/devtools/autopress.h"

using Core::Devtools::AutoPressDevice;
using Core::Devtools::AutoPressDevicesInclude;
using Core::Devtools::AutoPressInjector;
using Core::Devtools::AutoPressSchedule;
using Core::Devtools::ParseAutoPressDevices;

TEST(AutoPress, OffUnlessExplicitlyEnabled) {
    EXPECT_EQ(ParseAutoPressDevices(nullptr), AutoPressDevice::None);
    EXPECT_EQ(ParseAutoPressDevices(""), AutoPressDevice::None);
    EXPECT_EQ(ParseAutoPressDevices("0"), AutoPressDevice::None);
    EXPECT_EQ(ParseAutoPressDevices("true"), AutoPressDevice::None);
    EXPECT_EQ(ParseAutoPressDevices("1x"), AutoPressDevice::None);
}

TEST(AutoPress, ValueSelectsTheDevice) {
    EXPECT_EQ(ParseAutoPressDevices("1"), AutoPressDevice::All);
    EXPECT_EQ(ParseAutoPressDevices("all"), AutoPressDevice::All);
    EXPECT_EQ(ParseAutoPressDevices("pad"), AutoPressDevice::Pad);
    EXPECT_EQ(ParseAutoPressDevices("move"), AutoPressDevice::Move);
    EXPECT_TRUE(AutoPressDevicesInclude(AutoPressDevice::All, AutoPressDevice::Pad));
    EXPECT_TRUE(AutoPressDevicesInclude(AutoPressDevice::All, AutoPressDevice::Move));
    EXPECT_FALSE(AutoPressDevicesInclude(AutoPressDevice::Pad, AutoPressDevice::Move));
    EXPECT_FALSE(AutoPressDevicesInclude(AutoPressDevice::None, AutoPressDevice::Pad));
}

TEST(AutoPress, ScheduleIsBoundedAndCyclic) {
    AutoPressSchedule schedule;
    EXPECT_FALSE(schedule.Pressing(0));
    EXPECT_FALSE(schedule.Pressing(7999));
    EXPECT_TRUE(schedule.Pressing(9000));
    EXPECT_TRUE(schedule.Pressing(9500));
    EXPECT_FALSE(schedule.Pressing(9601));
    EXPECT_TRUE(schedule.Pressing(12000));
    EXPECT_FALSE(schedule.Pressing(44999)); // 44999 % 3000 = 2999
    EXPECT_FALSE(schedule.Pressing(45000)); // the window has ended
    EXPECT_FALSE(schedule.Pressing(60000));
}

TEST(AutoPress, InjectorReportsOneBurstStart) {
    AutoPressInjector injector(AutoPressDevice::Pad, /*start_ms=*/1000);
    bool began = true;
    EXPECT_FALSE(injector.Tick(2000, &began)); // 1000 ms elapsed
    EXPECT_FALSE(began);
    EXPECT_TRUE(injector.Tick(10000, &began)); // 9000 ms elapsed: first press
    EXPECT_TRUE(began);
    EXPECT_TRUE(injector.Tick(10100, &began)); // still in the same hold
    EXPECT_FALSE(began);
    EXPECT_FALSE(injector.Tick(10601, &began)); // hold ended
    EXPECT_FALSE(began);
    EXPECT_TRUE(injector.Tick(13000, &began)); // next cycle
    EXPECT_TRUE(began);
}

TEST(AutoPress, InjectorStopsAfterItsWindow) {
    AutoPressInjector injector(AutoPressDevice::Move, /*start_ms=*/0);
    EXPECT_FALSE(injector.Tick(46000));
    EXPECT_EQ(injector.elapsed_ms(46000), 46000u);
}

TEST(AutoPress, DisabledInjectorNeverPresses) {
    AutoPressInjector injector(AutoPressDevice::None, 0);
    EXPECT_FALSE(injector.enabled());
    bool began = true;
    for (u64 now = 0; now <= 60000; now += 100) {
        EXPECT_FALSE(injector.Tick(now, &began));
        EXPECT_FALSE(began);
    }
}
