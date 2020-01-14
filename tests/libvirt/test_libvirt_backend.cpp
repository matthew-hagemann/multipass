/*
 * Copyright (C) 2018-2019 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <src/platform/backends/libvirt/libvirt_virtual_machine_factory.h>

#include "tests/fake_handle.h"
#include "tests/mock_ssh.h"
#include "tests/mock_status_monitor.h"
#include "tests/stub_process_factory.h"
#include "tests/stub_ssh_key_provider.h"
#include "tests/stub_status_monitor.h"
#include "tests/temp_dir.h"
#include "tests/temp_file.h"

#include <multipass/memory_size.h>
#include <multipass/platform.h>
#include <multipass/virtual_machine.h>
#include <multipass/virtual_machine_description.h>

#include <cstdlib>

#include <gmock/gmock.h>

#include <multipass/format.h>

namespace mp = multipass;
namespace mpt = multipass::test;
using namespace testing;
using namespace std::chrono_literals;

struct LibVirtBackend : public Test
{
    mpt::TempFile dummy_image;
    mpt::TempFile dummy_cloud_init_iso;
    mp::VirtualMachineDescription default_description{2,
                                                      mp::MemorySize{"3M"},
                                                      mp::MemorySize{}, // not used
                                                      "pied-piper-valley",
                                                      "",
                                                      "",
                                                      {dummy_image.name(), "", "", "", "", "", "", {}},
                                                      dummy_cloud_init_iso.name()};
    mpt::TempDir data_dir;
    // This indicates that LibvirtWrapper should open the test executable
    std::string fake_libvirt_path{""};
};

TEST_F(LibVirtBackend, libvirt_wrapper_missing_libvirt_throws)
{
    EXPECT_THROW(mp::LibvirtWrapper{"missing_libvirt"}, mp::LibvirtOpenException);
}

TEST_F(LibVirtBackend, libvirt_wrapper_missing_symbol_throws)
{
    // Need to set LD_LIBRARY_PATH to this .so for the multipass_tests executable
    EXPECT_THROW(mp::LibvirtWrapper{"libbroken_libvirt.so"}, mp::LibvirtSymbolAddressException);
}

TEST_F(LibVirtBackend, health_check_failed_connection_throws)
{
    mp::LibVirtVirtualMachineFactory backend(data_dir.path(), fake_libvirt_path);
    backend.libvirt_wrapper->virConnectOpen = [](auto...) -> virConnectPtr { return nullptr; };

    EXPECT_THROW(backend.hypervisor_health_check(), std::runtime_error);
}

TEST_F(LibVirtBackend, creates_in_off_state)
{
    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};
    mpt::StubVMStatusMonitor stub_monitor;

    auto machine = backend.create_virtual_machine(default_description, stub_monitor);

    EXPECT_THAT(machine->current_state(), Eq(mp::VirtualMachine::State::off));
}

TEST_F(LibVirtBackend, creates_in_suspended_state_with_managed_save)
{
    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};
    backend.libvirt_wrapper->virDomainHasManagedSaveImage = [](auto...) { return 1; };

    mpt::StubVMStatusMonitor stub_monitor;
    auto machine = backend.create_virtual_machine(default_description, stub_monitor);

    EXPECT_THAT(machine->current_state(), Eq(mp::VirtualMachine::State::suspended));
}

TEST_F(LibVirtBackend, machine_sends_monitoring_events)
{
    REPLACE(ssh_connect, [](auto...) { return SSH_OK; });

    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};
    backend.libvirt_wrapper->virNetworkGetDHCPLeases = [](auto, auto, auto leases, auto) {
        virNetworkDHCPLeasePtr* leases_ret;
        leases_ret = (virNetworkDHCPLeasePtr*)calloc(1, sizeof(virNetworkDHCPLeasePtr));
        leases_ret[0] = (virNetworkDHCPLeasePtr)calloc(1, sizeof(virNetworkDHCPLease));
        leases_ret[0]->ipaddr = strdup("0.0.0.0");
        *leases = leases_ret;

        return 1;
    };

    NiceMock<mpt::MockVMStatusMonitor> mock_monitor;
    auto machine = backend.create_virtual_machine(default_description, mock_monitor);

    EXPECT_CALL(mock_monitor, on_resume());
    machine->start();

    backend.libvirt_wrapper->virDomainGetState = [](auto, auto state, auto, auto) {
        *state = VIR_DOMAIN_RUNNING;
        return 0;
    };

    machine->wait_until_ssh_up(2min);

    EXPECT_CALL(mock_monitor, on_shutdown());
    machine->shutdown();

    EXPECT_CALL(mock_monitor, on_suspend());
    machine->suspend();
}

TEST_F(LibVirtBackend, machine_persists_and_sets_state_on_start)
{
    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};
    NiceMock<mpt::MockVMStatusMonitor> mock_monitor;
    auto machine = backend.create_virtual_machine(default_description, mock_monitor);

    EXPECT_CALL(mock_monitor, persist_state_for(_, _));
    machine->start();

    backend.libvirt_wrapper->virDomainGetState = [](auto, auto state, auto, auto) {
        *state = VIR_DOMAIN_RUNNING;
        return 0;
    };

    EXPECT_THAT(machine->current_state(), Eq(mp::VirtualMachine::State::starting));
}

TEST_F(LibVirtBackend, machine_persists_and_sets_state_on_shutdown)
{
    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};
    backend.libvirt_wrapper->virDomainGetState = [](auto, auto state, auto, auto) {
        *state = VIR_DOMAIN_RUNNING;
        return 0;
    };

    NiceMock<mpt::MockVMStatusMonitor> mock_monitor;
    auto machine = backend.create_virtual_machine(default_description, mock_monitor);

    EXPECT_CALL(mock_monitor, persist_state_for(_, _));
    machine->shutdown();

    backend.libvirt_wrapper->virDomainGetState = [](auto, auto state, auto, auto) {
        *state = VIR_DOMAIN_SHUTOFF;
        return 0;
    };
    EXPECT_THAT(machine->current_state(), Eq(mp::VirtualMachine::State::off));
}

TEST_F(LibVirtBackend, machine_persists_and_sets_state_on_suspend)
{
    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};
    backend.libvirt_wrapper->virDomainGetState = [](auto, auto state, auto, auto) {
        *state = VIR_DOMAIN_RUNNING;
        return 0;
    };

    NiceMock<mpt::MockVMStatusMonitor> mock_monitor;
    auto machine = backend.create_virtual_machine(default_description, mock_monitor);

    EXPECT_CALL(mock_monitor, persist_state_for(_, _));
    machine->suspend();

    backend.libvirt_wrapper->virDomainGetState = [](auto, auto state, auto, auto) {
        *state = VIR_DOMAIN_SHUTOFF;
        return 0;
    };
    backend.libvirt_wrapper->virDomainHasManagedSaveImage = [](auto...) { return 1; };

    EXPECT_THAT(machine->current_state(), Eq(mp::VirtualMachine::State::suspended));
}

TEST_F(LibVirtBackend, start_with_broken_libvirt_connection_throws)
{
    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};
    backend.libvirt_wrapper->virConnectOpen = [](auto...) -> virConnectPtr { return nullptr; };

    NiceMock<mpt::MockVMStatusMonitor> mock_monitor;
    auto machine = backend.create_virtual_machine(default_description, mock_monitor);

    EXPECT_THROW(machine->start(), std::runtime_error);

    EXPECT_THAT(machine->current_state(), Eq(mp::VirtualMachine::State::unknown));
}

TEST_F(LibVirtBackend, shutdown_with_broken_libvirt_connection_throws)
{
    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};
    backend.libvirt_wrapper->virConnectOpen = [](auto...) -> virConnectPtr { return nullptr; };

    NiceMock<mpt::MockVMStatusMonitor> mock_monitor;
    auto machine = backend.create_virtual_machine(default_description, mock_monitor);

    EXPECT_THROW(machine->shutdown(), std::runtime_error);

    EXPECT_THAT(machine->current_state(), Eq(mp::VirtualMachine::State::unknown));
}

TEST_F(LibVirtBackend, suspend_with_broken_libvirt_connection_throws)
{
    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};
    backend.libvirt_wrapper->virConnectOpen = [](auto...) -> virConnectPtr { return nullptr; };

    NiceMock<mpt::MockVMStatusMonitor> mock_monitor;
    auto machine = backend.create_virtual_machine(default_description, mock_monitor);

    EXPECT_THROW(machine->suspend(), std::runtime_error);

    EXPECT_THAT(machine->current_state(), Eq(mp::VirtualMachine::State::unknown));
}

TEST_F(LibVirtBackend, current_state_with_broken_libvirt_unknown)
{
    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};
    backend.libvirt_wrapper->virConnectOpen = [](auto...) -> virConnectPtr { return nullptr; };

    NiceMock<mpt::MockVMStatusMonitor> mock_monitor;
    auto machine = backend.create_virtual_machine(default_description, mock_monitor);

    EXPECT_THAT(machine->current_state(), Eq(mp::VirtualMachine::State::unknown));
}

TEST_F(LibVirtBackend, current_state_delayed_shutdown_domain_running)
{
    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};
    backend.libvirt_wrapper->virDomainGetState = [](auto, auto state, auto, auto) {
        *state = VIR_DOMAIN_RUNNING;
        return 0;
    };

    NiceMock<mpt::MockVMStatusMonitor> mock_monitor;
    auto machine = backend.create_virtual_machine(default_description, mock_monitor);
    machine->state = mp::VirtualMachine::State::delayed_shutdown;

    EXPECT_THAT(machine->current_state(), Eq(mp::VirtualMachine::State::delayed_shutdown));
}

TEST_F(LibVirtBackend, current_state_delayed_shutdown_domain_off)
{
    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};
    NiceMock<mpt::MockVMStatusMonitor> mock_monitor;
    auto machine = backend.create_virtual_machine(default_description, mock_monitor);
    machine->state = mp::VirtualMachine::State::delayed_shutdown;

    EXPECT_THAT(machine->current_state(), Eq(mp::VirtualMachine::State::off));
}

TEST_F(LibVirtBackend, current_state_off_domain_starts_running)
{
    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};
    NiceMock<mpt::MockVMStatusMonitor> mock_monitor;
    auto machine = backend.create_virtual_machine(default_description, mock_monitor);

    EXPECT_THAT(machine->current_state(), Eq(mp::VirtualMachine::State::off));

    backend.libvirt_wrapper->virDomainGetState = [](auto, auto state, auto, auto) {
        *state = VIR_DOMAIN_RUNNING;
        return 0;
    };

    EXPECT_THAT(machine->current_state(), Eq(mp::VirtualMachine::State::running));
}

TEST_F(LibVirtBackend, returns_version_string)
{
    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};
    backend.libvirt_wrapper->virConnectGetVersion = [](virConnectPtr, unsigned long* hvVer) {
        *hvVer = 1002003;
        return 0;
    };

    EXPECT_EQ(backend.get_backend_version_string(), "libvirt-1.2.3");
}

TEST_F(LibVirtBackend, returns_version_string_when_error)
{
    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};
    backend.libvirt_wrapper->virConnectGetVersion = [](auto...) { return -1; };

    EXPECT_EQ(backend.get_backend_version_string(), "libvirt-unknown");
}

TEST_F(LibVirtBackend, returns_version_string_when_lacking_capabilities)
{
    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};

    EXPECT_EQ(backend.get_backend_version_string(), "libvirt-unknown");
}

TEST_F(LibVirtBackend, returns_version_string_when_failed_connecting)
{
    auto called{0};
    auto virConnectGetVersion = [&called](auto...) {
        ++called;
        return 0;
    };

    // Need this to "fake out" not being able to assign lambda's that capture directly
    // to a pointer to a function.
    static auto static_virConnectGetVersion = virConnectGetVersion;

    mp::LibVirtVirtualMachineFactory backend{data_dir.path(), fake_libvirt_path};
    backend.libvirt_wrapper->virConnectOpen = [](auto...) -> virConnectPtr { return nullptr; };
    backend.libvirt_wrapper->virConnectGetVersion = [](virConnectPtr conn, long unsigned int* hwVer) {
        return static_virConnectGetVersion(conn, hwVer);
    };

    EXPECT_EQ(backend.get_backend_version_string(), "libvirt-unknown");
    EXPECT_EQ(called, 0);
}