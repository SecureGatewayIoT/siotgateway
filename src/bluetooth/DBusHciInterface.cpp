#include <Poco/Exception.h>
#include <Poco/Logger.h>
#include <Poco/NumberFormatter.h>
#include <Poco/String.h>

#include "bluetooth/BluezHciInterface.h"
#include "bluetooth/DBusHciConnection.h"
#include "bluetooth/DBusHciInterface.h"
#include "di/Injectable.h"

BEEEON_OBJECT_BEGIN(BeeeOn, DBusHciInterfaceManager)
BEEEON_OBJECT_CASTABLE(HciInterfaceManager)
BEEEON_OBJECT_END(BeeeOn, DBusHciInterfaceManager)

using namespace BeeeOn;
using namespace Poco;
using namespace std;

static int CHANGE_POWER_ATTEMPTS = 5;
static Timespan CHANGE_POWER_DELAY = 200 * Timespan::MILLISECONDS;

DBusHciInterface::DBusHciInterface(const string& name):
	m_name(name),
	m_loopThread(*this, &DBusHciInterface::runLoop)
{
	m_adapter = retrieveBluezAdapter(createAdapterPath(m_name));
	m_thread.start(m_loopThread);
}

DBusHciInterface::~DBusHciInterface()
{
	stopDiscovery(m_adapter);

	if (::g_main_loop_is_running(m_loop.raw()))
		::g_main_loop_quit(m_loop.raw());

	try {
		m_thread.join();
	}
	BEEEON_CATCH_CHAIN(logger())
}

/**
 * Convert the given GError into the appropriate exception and throw it.
 */
static void throwErrorIfAny(const GlibPtr<GError> error)
{
	if (!error.isNull())
		throw IOException(error->message);
}

void DBusHciInterface::up() const
{
	if (logger().debug())
		logger().debug("bringing up " + m_name, __FILE__, __LINE__);

	ScopedLock<FastMutex> guard(m_statusMutex);

	const string path = createAdapterPath(m_name);
	GlibPtr<OrgBluezAdapter1> adapter = retrieveBluezAdapter(path);

	startDiscovery(m_adapter, "le");

	if (::org_bluez_adapter1_get_powered(adapter.raw()))
		return;

	::org_bluez_adapter1_set_powered(adapter.raw(), true);
	waitUntilPoweredChange(path, true);
}

void DBusHciInterface::down() const
{
	if (logger().debug())
		logger().debug("switching down " + m_name, __FILE__, __LINE__);

	ScopedLock<FastMutex> guard(m_statusMutex);

	m_waitCondition.broadcast();

	const string path = createAdapterPath(m_name);
	GlibPtr<OrgBluezAdapter1> adapter = retrieveBluezAdapter(path);

	if (!::org_bluez_adapter1_get_powered(adapter.raw()))
		return;

	::org_bluez_adapter1_set_powered(adapter.raw(), false);
	waitUntilPoweredChange(path, false);
}

void DBusHciInterface::reset() const
{
	down();
	up();
}

bool DBusHciInterface::detect(const MACAddress &address) const
{
	BluezHciInterface bluezHci(m_name);
	return bluezHci.detect(address);
}

map<MACAddress, string> DBusHciInterface::scan() const
{
	BluezHciInterface bluezHci(m_name);
	return bluezHci.scan();
}

map<MACAddress, string> DBusHciInterface::lescan(const Timespan& timeout) const
{
	logger().information("starting BLE scan for " +
		to_string(timeout.totalSeconds()) + " seconds", __FILE__, __LINE__);

	GlibPtr<GDBusObjectManager> objectManager = createBluezObjectManager();

	map<MACAddress, string> allDevices;
	processKnownDevices(objectManager, allDevices);

	::g_signal_connect(
		G_DBUS_OBJECT_MANAGER(objectManager.raw()),
		"object-added",
		G_CALLBACK(onDBusObjectAdded),
		&allDevices);

	startDiscovery(m_adapter, "le");

	m_waitCondition.tryWait(timeout);

	map<MACAddress, string> foundDevices;
	for (auto one : allDevices) {
		GlibPtr<OrgBluezDevice1> device = retrieveBluezDevice(createDevicePath(m_name, one.first));
		int16_t rssi = ::org_bluez_device1_get_rssi(device.raw());
		if (rssi != 0) {
			foundDevices.emplace(one.first, one.second);

			if (logger().debug())
				logger().debug("found BLE device " + one.second + " by address " + one.first.toString(':') +
					" (" + to_string(rssi) + ")", __FILE__, __LINE__);
		}
	}

	logger().information("BLE scan has finished, found " +
		to_string(foundDevices.size()) + " device(s)", __FILE__, __LINE__);

	return foundDevices;
}

HciInfo DBusHciInterface::info() const
{
	BluezHciInterface bluezHci(m_name);
	return bluezHci.info();
}

HciConnection::Ptr DBusHciInterface::connect(
		const MACAddress& address,
		const Timespan& timeout) const
{
	if (logger().debug())
		logger().debug("connecting to device " + address.toString(':'), __FILE__, __LINE__);

	const string path = createDevicePath(m_name, address);
	GlibPtr<OrgBluezDevice1> device = retrieveBluezDevice(path);

	if (!::org_bluez_device1_get_connected(device.raw())) {
		GlibPtr<GError> error;
		::g_dbus_proxy_set_default_timeout(G_DBUS_PROXY(device.raw()), timeout.totalMilliseconds());
		::org_bluez_device1_call_connect_sync(device.raw(), nullptr, &error);

		throwErrorIfAny(error);
	}

	return new DBusHciConnection(m_name, device, timeout);
}

void DBusHciInterface::waitUntilPoweredChange(const string& path, const bool powered) const
{
	for (int i = 0; i < CHANGE_POWER_ATTEMPTS; ++i) {
		GlibPtr<OrgBluezAdapter1> adapter = retrieveBluezAdapter(path);
		if (::org_bluez_adapter1_get_powered(adapter.raw()) == powered)
			return;

		m_condition.tryWait(m_statusMutex, CHANGE_POWER_DELAY.totalMilliseconds());
	}

	throw TimeoutException("failed to change power of interface" + m_name);
}

void DBusHciInterface::startDiscovery(
		GlibPtr<OrgBluezAdapter1> adapter,
		const std::string& trasport) const
{
	ScopedLock<FastMutex> guard(m_discoveringMutex);

	if (::org_bluez_adapter1_get_discovering(adapter.raw()))
		return;

	initDiscoveryFilter(adapter, trasport);
	::org_bluez_adapter1_call_start_discovery_sync(adapter.raw(), nullptr, nullptr);
}

void DBusHciInterface::stopDiscovery(GlibPtr<OrgBluezAdapter1> adapter) const
{
	ScopedLock<FastMutex> guard(m_discoveringMutex);

	if (!::org_bluez_adapter1_get_discovering(adapter.raw()))
		return;

	::org_bluez_adapter1_call_stop_discovery_sync(adapter.raw(), nullptr, nullptr);
}

void DBusHciInterface::initDiscoveryFilter(
		GlibPtr<OrgBluezAdapter1> adapter,
		const string& trasport) const
{
	GlibPtr<GError> error;
	GVariantBuilder args;
	::g_variant_builder_init(&args, G_VARIANT_TYPE("a{sv}"));
	::g_variant_builder_add(&args, "{sv}", "Transport", g_variant_new_string(trasport.c_str()));
	::org_bluez_adapter1_call_set_discovery_filter_sync(
		adapter.raw(), ::g_variant_builder_end(&args), nullptr, &error);
	throwErrorIfAny(error);
}

void DBusHciInterface::processKnownDevices(
		GlibPtr<GDBusObjectManager> objectManager,
		map<MACAddress, string>& devices) const
{
	GlibPtr<GError> error;
	PathFilter pathFilter =
		[&](const string& path)-> bool {
			if (path.find("/" + m_name) == string::npos)
				return true;
			else
				return false;
		};

	for (auto path : retrievePathsOfBluezObjects(objectManager, pathFilter, "org.bluez.Device1")) {
		GlibPtr<OrgBluezDevice1> device;
		try {
			device = retrieveBluezDevice(path.c_str());
		}
		BEEEON_CATCH_CHAIN_ACTION(logger(),
			continue);

		MACAddress mac = MACAddress::parse(::org_bluez_device1_get_address(device.raw()), ':');
		const char* charName = ::org_bluez_device1_get_name(device.raw());
		const string name = charName == nullptr ? "unknown" : charName;
		devices.emplace(mac, name);
	}
}

void DBusHciInterface::runLoop()
{
	m_loop = ::g_main_loop_new(nullptr, false);
	::g_main_loop_run(m_loop.raw());
}

vector<string> DBusHciInterface::retrievePathsOfBluezObjects(
		GlibPtr<GDBusObjectManager> objectManager,
		PathFilter pathFilter,
		const std::string& objectFilter)
{
	vector<string> paths;
	GlibPtr<GList> objects = ::g_dbus_object_manager_get_objects(objectManager.raw());

	for (GList* l = objects.raw(); l != nullptr; l = l->next) {
		const string objectPath = ::g_dbus_object_get_object_path(G_DBUS_OBJECT(l->data));

		// Example of input: /org/bluez/hci0/dev_FF_FF_FF_FF_FF_FF
		if (pathFilter(objectPath))
			continue;

		GlibPtr<GDBusInterface> interface =
			::g_dbus_object_manager_get_interface(objectManager.raw(), objectPath.c_str(), objectFilter.c_str());
		if (interface.isNull())
			continue;

		paths.emplace_back(objectPath);
	}

	return paths;
}

gboolean DBusHciInterface::onStopLoop(gpointer loop)
{
	::g_main_loop_quit(reinterpret_cast<GMainLoop*>(loop));
	return false;
}

void DBusHciInterface::onDBusObjectAdded(
		GDBusObjectManager* objectManager,
		GDBusObject* object,
		gpointer userData)
{
	const string path = ::g_dbus_object_get_object_path(G_DBUS_OBJECT(object));
	GlibPtr<GDBusInterface> interface =
		::g_dbus_object_manager_get_interface(objectManager, path.c_str(), "org.bluez.Device1");
	if (interface.isNull())
		return;

	GlibPtr<OrgBluezDevice1> device;
	try {
		device = retrieveBluezDevice(path);
	}
	BEEEON_CATCH_CHAIN_ACTION(Loggable::forClass(typeid(DBusHciInterface)),
		return);

	MACAddress mac = MACAddress::parse(::org_bluez_device1_get_address(device.raw()), ':');
	const char* charName = ::org_bluez_device1_get_name(device.raw());
	const string name = charName == nullptr ? "unknown" : charName;

	map<MACAddress, string> &foundDevices = *(reinterpret_cast<map<MACAddress, string>*>(userData));
	foundDevices.emplace(mac, name);
}

const string DBusHciInterface::createAdapterPath(const string& name)
{
	return "/org/bluez/" + name;
}

const string DBusHciInterface::createDevicePath(const string& name, const MACAddress& address)
{
	return "/org/bluez/" + name + "/dev_" + address.toString('_');
}

GlibPtr<GDBusObjectManager> DBusHciInterface::createBluezObjectManager()
{
	GlibPtr<GError> error;

	GlibPtr<GDBusObjectManager> objectManager = ::g_dbus_object_manager_client_new_for_bus_sync(
		G_BUS_TYPE_SYSTEM,
		G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
		"org.bluez",
		"/",
		nullptr, nullptr, nullptr, nullptr,
		&error);

	throwErrorIfAny(error);
	return objectManager;
}

GlibPtr<OrgBluezAdapter1> DBusHciInterface::retrieveBluezAdapter(const string& path)
{
	GlibPtr<GError> error;

	GlibPtr<OrgBluezAdapter1> adapter = ::org_bluez_adapter1_proxy_new_for_bus_sync(
		G_BUS_TYPE_SYSTEM,
		G_DBUS_PROXY_FLAGS_NONE,
		"org.bluez",
		path.c_str(),
		nullptr,
		&error);

	throwErrorIfAny(error);
	return adapter;
}

GlibPtr<OrgBluezDevice1> DBusHciInterface::retrieveBluezDevice(const string& path)
{
	GlibPtr<GError> error;

	GlibPtr<OrgBluezDevice1> device = ::org_bluez_device1_proxy_new_for_bus_sync(
		G_BUS_TYPE_SYSTEM,
		G_DBUS_PROXY_FLAGS_NONE,
		"org.bluez",
		path.c_str(),
		nullptr,
		&error);

	throwErrorIfAny(error);
	return device;
}


DBusHciInterfaceManager::DBusHciInterfaceManager()
{
}

HciInterface::Ptr DBusHciInterfaceManager::lookup(const string &name)
{
	auto it = m_interfaces.find(name);
	if (it != m_interfaces.end())
		return it->second;

	DBusHciInterface::Ptr newHci = new DBusHciInterface(name);
	m_interfaces.emplace(name, newHci);
	return newHci;
}
