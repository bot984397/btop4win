/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

indent = tab
tab-size = 4
*/

#include <fstream>
#include <ranges>
#include <cmath>
#include <numeric>

#define _WIN32_WINNT 0x0600
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <windows.h>

#include <winreg.h>
#pragma comment( lib, "Advapi32.lib" )
#include <winternl.h>
#pragma comment( lib, "ntdll.lib" )
#include <Pdh.h>
#pragma comment( lib, "Pdh.lib" )
#include <powerbase.h>
#pragma comment( lib, "PowrProf.lib")
#include <atlstr.h>
#include <tlhelp32.h>

#include <btop_shared.hpp>
#include <btop_config.hpp>
#include <btop_tools.hpp>

using std::ifstream, std::numeric_limits, std::streamsize, std::round, std::max, std::min;
using std::clamp, std::string_literals::operator""s, std::cmp_equal, std::cmp_less, std::cmp_greater;
namespace fs = std::filesystem;
namespace rng = std::ranges;
using namespace Tools;

//? --------------------------------------------------- FUNCTIONS -----------------------------------------------------

namespace Tools {
	class HandleWrapper {
	public:
		HANDLE wHandle;
		bool valid = false;
		HandleWrapper() { ; }
		HandleWrapper(HANDLE nHandle) : wHandle(nHandle) { valid = (wHandle != INVALID_HANDLE_VALUE && wHandle != NULL); }
		auto operator()() { return wHandle; }
		~HandleWrapper() { CloseHandle(wHandle); }
	};

	//? Set security modes for better chance of collecting process information
	//? Based on code from psutil
	//? See: https://github.com/giampaolo/psutil/blob/master/psutil/arch/windows/security.c
	void setWinDebug() {
		HandleWrapper hToken{};
		HANDLE thisProc = GetCurrentProcess();

		if (not OpenProcessToken(thisProc, TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken.wHandle)) {
			if (GetLastError() == ERROR_NO_TOKEN) {
				if (not ImpersonateSelf(SecurityImpersonation))
					throw std::runtime_error("setWinDebug() -> ImpersonateSelf() failed with ID: " + to_string(GetLastError()));
				if (not OpenProcessToken(thisProc, TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken.wHandle))
					throw std::runtime_error("setWinDebug() -> OpenProcessToken() failed with ID: " + to_string(GetLastError()));
			}
			else
				throw std::runtime_error("setWinDebug() -> OpenProcessToken() failed with ID: " + to_string(GetLastError()));
		}

		TOKEN_PRIVILEGES tpriv;
		TOKEN_PRIVILEGES old_tpriv;
		LUID luid;
		DWORD old_tprivSize = sizeof(TOKEN_PRIVILEGES);

		if (not LookupPrivilegeValue(0, SE_DEBUG_NAME, &luid))
			throw std::runtime_error("setWinDebug() -> LookupPrivilegeValue() failed with ID: " + to_string(GetLastError()));

		tpriv.PrivilegeCount = 1;
		tpriv.Privileges[0].Luid = luid;
		tpriv.Privileges[0].Attributes = 0;

		if (not AdjustTokenPrivileges(hToken(), FALSE, &tpriv, sizeof(TOKEN_PRIVILEGES), &old_tpriv, &old_tprivSize))
			throw std::runtime_error("setWinDebug() -> AdjustTokenPrivileges() [get] failed with ID: " + to_string(GetLastError()));

		old_tpriv.PrivilegeCount = 1;
		old_tpriv.Privileges[0].Luid = luid;
		old_tpriv.Privileges[0].Attributes |= (SE_PRIVILEGE_ENABLED);

		if (not AdjustTokenPrivileges(hToken(), FALSE, &old_tpriv, old_tprivSize, 0, 0))
			throw std::runtime_error("setWinDebug() -> AdjustTokenPrivileges() [set] failed with ID: " + to_string(GetLastError()));

		RevertToSelf();
	}
}

namespace Cpu {
	vector<long long> core_old_totals;
	vector<long long> core_old_idles;
	vector<string> available_fields;
	vector<string> available_sensors = {"Auto"};
	cpu_info current_cpu;
	bool got_sensors = false, cpu_temp_only = false;

	//* Populate found_sensors map
	bool get_sensors();

	//* Get current cpu clock speed
	string get_cpuHz();

	//* Search /proc/cpuinfo for a cpu name
	string get_cpuName();

	struct Sensor {
		fs::path path;
		string label;
		int64_t temp = 0;
		int64_t high = 0;
		int64_t crit = 0;
	};

	unordered_flat_map<string, Sensor> found_sensors;
	string cpu_sensor;
	vector<string> core_sensors;
	unordered_flat_map<int, int> core_mapping;
}

namespace Mem {
	double old_uptime;
}

namespace Cpu {

	// Code for load average comes from psutils calculation
	// see https://github.com/giampaolo/psutil/blob/master/psutil/arch/windows/wmi.c

	const double LAVG_1F = 0.9200444146293232478931553241;
	const double LAVG_5F = 0.9834714538216174894737477501;
	const double LAVG_15F = 0.9944598480048967508795473394;
	double load_avg_1m = 0.0;
	double load_avg_5m = 0.0;
	double load_avg_15m = 0.0;

	HQUERY hQuery;
	HCOUNTER hCounter;
	HANDLE eventh;
	HANDLE waitHandle;
	DWORD sample_i = 5;

	void CALLBACK LoadAvgCallback(PVOID hCounter, BOOLEAN timedOut) {
		PDH_FMT_COUNTERVALUE displayValue;
		double currentLoad;
		
		if (PdhGetFormattedCounterValue((PDH_HCOUNTER)hCounter, PDH_FMT_DOUBLE, 0, &displayValue) != ERROR_SUCCESS) {
			return;
		}
		currentLoad = displayValue.doubleValue;

		load_avg_1m = load_avg_1m * LAVG_1F + currentLoad * (1.0 - LAVG_1F);
		load_avg_5m = load_avg_5m * LAVG_5F + currentLoad * (1.0 - LAVG_5F);
		load_avg_15m = load_avg_15m * LAVG_15F + currentLoad * (1.0 - LAVG_15F);
	}

	void loadAVG_init() {
		if (PdhOpenQueryW(nullptr, 0, &hQuery) != ERROR_SUCCESS) {
			throw std::runtime_error("Cpu::loadAVG_init() -> PdhOpenQueryW failed");
		}

		if (PdhAddEnglishCounterW(hQuery, L"\\System\\Processor Queue Length", 0, &hCounter) != ERROR_SUCCESS) {
			throw std::runtime_error("Cpu::loadAVG_init() -> PdhAddEnglishCounterW failed");
		}

		eventh = CreateEventW(NULL, FALSE, FALSE, L"LoadUpdateEvent");
		if (eventh == NULL) {
			throw std::runtime_error("Cpu::loadAVG_init() -> CreateEventW failed");
		}

		if (PdhCollectQueryDataEx(hQuery, sample_i, eventh) != ERROR_SUCCESS) {
			throw std::runtime_error("Cpu::loadAVG_init() -> PdhCollectQueryDataEx failed");
		}

		if (RegisterWaitForSingleObject(&waitHandle, eventh, (WAITORTIMERCALLBACK)LoadAvgCallback, (PVOID)hCounter, INFINITE, WT_EXECUTEDEFAULT) == 0) {
			throw std::runtime_error("Cpu::loadAVG_init() -> RegisterWaitForSingleObject failed");
		}
	}
}

namespace Shared {

	fs::path procPath, passwd_path;
	long pageSize, clkTck, coreCount;

	void init() {

		//? Shared global variables init
		procPath = "";
		passwd_path = "";

		SYSTEM_INFO sysinfo;
		GetSystemInfo(&sysinfo);

		coreCount = sysinfo.dwNumberOfProcessors;
		if (coreCount < 1) {
			throw std::runtime_error("Could not determine number of cores!");
		}

		pageSize = sysinfo.dwPageSize;
		if (pageSize <= 0) {
			pageSize = 4096;
			Logger::warning("Could not get system page size. Defaulting to 4096, processes memory usage might be incorrect.");
		}

		clkTck = 100;
		if (clkTck <= 0) {
			clkTck = 100;
			Logger::warning("Could not get system clock ticks per second. Defaulting to 100, processes cpu usage might be incorrect.");
		}

		//? Init for namespace Cpu
		Cpu::current_cpu.core_percent.insert(Cpu::current_cpu.core_percent.begin(), Shared::coreCount, {});
		Cpu::current_cpu.temp.insert(Cpu::current_cpu.temp.begin(), Shared::coreCount + 1, {});
		Cpu::core_old_totals.insert(Cpu::core_old_totals.begin(), Shared::coreCount, 0);
		Cpu::core_old_idles.insert(Cpu::core_old_idles.begin(), Shared::coreCount, 0);
		Cpu::collect();
		for (auto& [field, vec] : Cpu::current_cpu.cpu_percent) {
			if (not vec.empty()) Cpu::available_fields.push_back(field);
		}
		Cpu::cpuName = Cpu::get_cpuName();
		Cpu::got_sensors = Cpu::get_sensors();
		for (const auto& [sensor, ignored] : Cpu::found_sensors) {
			Cpu::available_sensors.push_back(sensor);
		}
		Cpu::core_mapping = Cpu::get_core_mapping();

		std::thread(Cpu::loadAVG_init).detach();

		//? Init for namespace Mem
		Mem::old_uptime = system_uptime();
		Mem::collect();

		try {
			setWinDebug();
		}
		catch (const std::exception& e) {
			Logger::warning("Failed to set SE DEBUG mode for process!");
			Logger::debug(e.what());
		}

	}

}

namespace Cpu {
	string cpuName;
	string cpuHz;
	bool has_battery = true;
	tuple<int, long, string> current_bat;

	const array<string, 6> time_names = { "kernel", "user", "dpc", "interrupt", "idle" };

	unordered_flat_map<string, long long> cpu_old = {
			{"total", 0},
			{"kernel", 0},
			{"user", 0},
			{"dpc", 0},
			{"interrupt", 0},
			{"idle", 0},
			{"totals", 0},
			{"idles", 0}
		};

	string get_cpuName() {
		string name;
		HKEY hKey;

		if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
			wchar_t cpuName[255];
			DWORD BufSize = sizeof(cpuName);
			if (RegQueryValueEx(hKey, L"ProcessorNameString", nullptr, nullptr, (LPBYTE)cpuName, &BufSize) == ERROR_SUCCESS) {
				name = string(CW2A(cpuName));
			}
		}

		if (name.empty()) return "";

		auto name_vec = ssplit(name);

		if ((s_contains(name, "Xeon"s) or v_contains(name_vec, "Duo"s)) and v_contains(name_vec, "CPU"s)) {
			auto cpu_pos = v_index(name_vec, "CPU"s);
			if (cpu_pos < name_vec.size() - 1 and not name_vec.at(cpu_pos + 1).ends_with(')'))
				name = name_vec.at(cpu_pos + 1);
			else
				name.clear();
		}
		else if (v_contains(name_vec, "Ryzen"s)) {
			auto ryz_pos = v_index(name_vec, "Ryzen"s);
			name = "Ryzen"	+ (ryz_pos < name_vec.size() - 1 ? ' ' + name_vec.at(ryz_pos + 1) : "")
							+ (ryz_pos < name_vec.size() - 2 ? ' ' + name_vec.at(ryz_pos + 2) : "");
		}
		else if (s_contains(name, "Intel"s) and v_contains(name_vec, "CPU"s)) {
			auto cpu_pos = v_index(name_vec, "CPU"s);
			if (cpu_pos < name_vec.size() - 1 and not name_vec.at(cpu_pos + 1).ends_with(')') and name_vec.at(cpu_pos + 1) != "@")
				name = name_vec.at(cpu_pos + 1);
			else
				name.clear();
		}
		else
			name.clear();

		if (name.empty() and not name_vec.empty()) {
			for (const auto& n : name_vec) {
				if (n == "@") break;
				name += n + ' ';
			}
			name.pop_back();
			for (const auto& replace : {"Processor", "CPU", "(R)", "(TM)", "Intel", "AMD", "Core"}) {
				name = s_replace(name, replace, "");
				name = s_replace(name, "  ", " ");
			}
			name = trim(name);
		}
		

		return name;
	}

	bool get_sensors() {
		bool got_cpu = false, got_coretemp = false;
		vector<fs::path> search_paths;
		//try {
		//	//? Setup up paths to search for sensors
		//	if (fs::exists(fs::path("/sys/class/hwmon")) and access("/sys/class/hwmon", R_OK) != -1) {
		//		for (const auto& dir : fs::directory_iterator(fs::path("/sys/class/hwmon"))) {
		//			fs::path add_path = fs::canonical(dir.path());
		//			if (v_contains(search_paths, add_path) or v_contains(search_paths, add_path / "device")) continue;

		//			if (s_contains(add_path, "coretemp"))
		//				got_coretemp = true;

		//			for (const auto & file : fs::directory_iterator(add_path)) {
		//				if (string(file.path().filename()) == "device") {
		//					for (const auto & dev_file : fs::directory_iterator(file.path())) {
		//						string dev_filename = dev_file.path().filename();
		//						if (dev_filename.starts_with("temp") and dev_filename.ends_with("_input")) {
		//							search_paths.push_back(file.path());
		//							break;
		//						}
		//					}
		//				}

		//				string filename = file.path().filename();
		//				if (filename.starts_with("temp") and filename.ends_with("_input")) {
		//					search_paths.push_back(add_path);
		//					break;
		//				}
		//			}
		//		}
		//	}
		//	if (not got_coretemp and fs::exists(fs::path("/sys/devices/platform/coretemp.0/hwmon"))) {
		//		for (auto& d : fs::directory_iterator(fs::path("/sys/devices/platform/coretemp.0/hwmon"))) {
		//			fs::path add_path = fs::canonical(d.path());

		//			for (const auto & file : fs::directory_iterator(add_path)) {
		//				string filename = file.path().filename();
		//				if (filename.starts_with("temp") and filename.ends_with("_input") and not v_contains(search_paths, add_path)) {
		//						search_paths.push_back(add_path);
		//						got_coretemp = true;
		//						break;
		//				}
		//			}
		//		}
		//	}
		//	//? Scan any found directories for temperature sensors
		//	if (not search_paths.empty()) {
		//		for (const auto& path : search_paths) {
		//			const string pname = readfile(path / "name", path.filename());
		//			for (const auto & file : fs::directory_iterator(path)) {
		//				const string file_suffix = "input";
		//				const int file_id = atoi(file.path().filename().c_str() + 4); // skip "temp" prefix
		//				string file_path = file.path();

		//				if (!s_contains(file_path, file_suffix)) {
		//					continue;
		//				}

		//				const string basepath = file_path.erase(file_path.find(file_suffix), file_suffix.length());
		//				const string label = readfile(fs::path(basepath + "label"), "temp" + to_string(file_id));
		//				const string sensor_name = pname + "/" + label;
		//				const int64_t temp = stol(readfile(fs::path(basepath + "input"), "0")) / 1000;
		//				const int64_t high = stol(readfile(fs::path(basepath + "max"), "80000")) / 1000;
		//				const int64_t crit = stol(readfile(fs::path(basepath + "crit"), "95000")) / 1000;

		//				found_sensors[sensor_name] = {fs::path(basepath + "input"), label, temp, high, crit};

		//				if (not got_cpu and (label.starts_with("Package id") or label.starts_with("Tdie"))) {
		//					got_cpu = true;
		//					cpu_sensor = sensor_name;
		//				}
		//				else if (label.starts_with("Core") or label.starts_with("Tccd")) {
		//					got_coretemp = true;
		//					if (not v_contains(core_sensors, sensor_name)) core_sensors.push_back(sensor_name);
		//				}
		//			}
		//		}
		//	}
		//	//? If no good candidate for cpu temp has been found scan /sys/class/thermal
		//	if (not got_cpu and fs::exists(fs::path("/sys/class/thermal"))) {
		//		const string rootpath = fs::path("/sys/class/thermal/thermal_zone");
		//		for (int i = 0; fs::exists(fs::path(rootpath + to_string(i))); i++) {
		//			const fs::path basepath = rootpath + to_string(i);
		//			if (not fs::exists(basepath / "temp")) continue;
		//			const string label = readfile(basepath / "type", "temp" + to_string(i));
		//			const string sensor_name = "thermal" + to_string(i) + "/" + label;
		//			const int64_t temp = stol(readfile(basepath / "temp", "0")) / 1000;

		//			int64_t high, crit;
		//			for (int ii = 0; fs::exists(basepath / string("trip_point_" + to_string(ii) + "_temp")); ii++) {
		//				const string trip_type = readfile(basepath / string("trip_point_" + to_string(ii) + "_type"));
		//				if (not is_in(trip_type, "high", "critical")) continue;
		//				auto& val = (trip_type == "high" ? high : crit);
		//				val = stol(readfile(basepath / string("trip_point_" + to_string(ii) + "_temp"), "0")) / 1000;
		//			}
		//			if (high < 1) high = 80;
		//			if (crit < 1) crit = 95;

		//			found_sensors[sensor_name] = {basepath / "temp", label, temp, high, crit};
		//		}
		//	}

		//}
		//catch (...) {}

		//if (not got_coretemp or core_sensors.empty()) {
		//	cpu_temp_only = true;
		//}
		//else {
		//	rng::sort(core_sensors, rng::less{});
		//	rng::stable_sort(core_sensors, [](const auto& a, const auto& b){
		//		return a.size() < b.size();
		//	});
		//}

		//if (cpu_sensor.empty() and not found_sensors.empty()) {
		//	for (const auto& [name, sensor] : found_sensors) {
		//		if (s_contains(str_to_lower(name), "cpu") or s_contains(str_to_lower(name), "k10temp")) {
		//			cpu_sensor = name;
		//			break;
		//		}
		//	}
		//	if (cpu_sensor.empty()) {
		//		cpu_sensor = found_sensors.begin()->first;
		//		Logger::warning("No good candidate for cpu sensor found, using random from all found sensors.");
		//	}
		//}

		return not found_sensors.empty();
	}

	void update_sensors() {
		if (cpu_sensor.empty()) return;

		//const auto& cpu_sensor = (not Config::getS("cpu_sensor").empty() and found_sensors.contains(Config::getS("cpu_sensor")) ? Config::getS("cpu_sensor") : Cpu::cpu_sensor);

		//found_sensors.at(cpu_sensor).temp = stol(readfile(found_sensors.at(cpu_sensor).path, "0")) / 1000;
		//current_cpu.temp.at(0).push_back(found_sensors.at(cpu_sensor).temp);
		//current_cpu.temp_max = found_sensors.at(cpu_sensor).crit;
		//if (current_cpu.temp.at(0).size() > 20) current_cpu.temp.at(0).pop_front();

		//if (Config::getB("show_coretemp") and not cpu_temp_only) {
		//	vector<string> done;
		//	for (const auto& sensor : core_sensors) {
		//		if (v_contains(done, sensor)) continue;
		//		found_sensors.at(sensor).temp = stol(readfile(found_sensors.at(sensor).path, "0")) / 1000;
		//		done.push_back(sensor);
		//	}
		//	for (const auto& [core, temp] : core_mapping) {
		//		if (cmp_less(core + 1, current_cpu.temp.size()) and cmp_less(temp, core_sensors.size())) {
		//			current_cpu.temp.at(core + 1).push_back(found_sensors.at(core_sensors.at(temp)).temp);
		//			if (current_cpu.temp.at(core + 1).size() > 20) current_cpu.temp.at(core + 1).pop_front();
		//		}
		//	}
		//}
	}

	typedef struct _PROCESSOR_POWER_INFORMATION {
		ULONG Number;
		ULONG MaxMhz;
		ULONG CurrentMhz;
		ULONG MhzLimit;
		ULONG MaxIdleState;
		ULONG CurrentIdleState;
	} PROCESSOR_POWER_INFORMATION, * PPROCESSOR_POWER_INFORMATION;

	string get_cpuHz() {
		static bool failed = false;
		if (failed) return "";
		vector<PROCESSOR_POWER_INFORMATION> ppinfo(Shared::coreCount);

		if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, &ppinfo[0], Shared::coreCount * sizeof(PROCESSOR_POWER_INFORMATION)) != 0) {
			Logger::warning("Cpu::get_cpuHz() -> CallNtPowerInformation() failed");
			failed = true;
			return "";
		}

		long hz = ppinfo[0].CurrentMhz;

		if (hz <= 1 or hz >= 1000000) {
			Logger::warning("Cpu::get_cpuHz() -> Got invalid cpu mhz value");
			failed = true;
			return "";
		}

		string cpuhz;
		if (hz >= 1000) {
			if (hz >= 10000) cpuhz = to_string((int)round(hz / 1000));
			else cpuhz = to_string(round(hz / 100) / 10.0).substr(0, 3);
			cpuhz += " GHz";
		}
		else if (hz > 0)
			cpuhz = to_string((int)round(hz)) + " MHz";

		return cpuhz;
	}

	auto get_core_mapping() -> unordered_flat_map<int, int> {
		unordered_flat_map<int, int> core_map;
		if (cpu_temp_only) return core_map;

		//? Try to get core mapping from /proc/cpuinfo
		//ifstream cpuinfo(Shared::procPath / "cpuinfo");
		//if (cpuinfo.good()) {
		//	int cpu, core, n = 0;
		//	for (string instr; cpuinfo >> instr;) {
		//		if (instr == "processor") {
		//			cpuinfo.ignore(SSmax, ':');
		//			cpuinfo >> cpu;
		//		}
		//		else if (instr.starts_with("core")) {
		//			cpuinfo.ignore(SSmax, ':');
		//			cpuinfo >> core;
		//			if (std::cmp_greater_equal(core, core_sensors.size())) {
		//				if (std::cmp_greater_equal(n, core_sensors.size())) n = 0;
		//				core_map[cpu] = n++;
		//			}
		//			else
		//				core_map[cpu] = core;
		//		}
		//		cpuinfo.ignore(SSmax, '\n');
		//	}
		//}

		////? If core mapping from cpuinfo was incomplete try to guess remainder, if missing completely, map 0-0 1-1 2-2 etc.
		//if (cmp_less(core_map.size(), Shared::coreCount)) {
		//	if (Shared::coreCount % 2 == 0 and (long)core_map.size() == Shared::coreCount / 2) {
		//		for (int i = 0, n = 0; i < Shared::coreCount / 2; i++) {
		//			if (std::cmp_greater_equal(n, core_sensors.size())) n = 0;
		//			core_map[Shared::coreCount / 2 + i] = n++;
		//		}
		//	}
		//	else {
		//		core_map.clear();
		//		for (int i = 0, n = 0; i < Shared::coreCount; i++) {
		//			if (std::cmp_greater_equal(n, core_sensors.size())) n = 0;
		//			core_map[i] = n++;
		//		}
		//	}
		//}

		////? Apply user set custom mapping if any
		//const auto& custom_map = Config::getS("cpu_core_map");
		//if (not custom_map.empty()) {
		//	try {
		//		for (const auto& split : ssplit(custom_map)) {
		//			const auto vals = ssplit(split, ':');
		//			if (vals.size() != 2) continue;
		//			int change_id = std::stoi(vals.at(0));
		//			int new_id = std::stoi(vals.at(1));
		//			if (not core_map.contains(change_id) or cmp_greater(new_id, core_sensors.size())) continue;
		//			core_map.at(change_id) = new_id;
		//		}
		//	}
		//	catch (...) {}
		//}

		return core_map;
	}

	struct battery {
		fs::path base_dir, energy_now, energy_full, power_now, status, online;
		string device_type;
		bool use_energy = true;
	};

	auto get_battery() -> tuple<int, long, string> {
		if (not has_battery) return {0, 0, ""};
		//static string auto_sel;
		//static unordered_flat_map<string, battery> batteries;

		////? Get paths to needed files and check for valid values on first run
		//if (batteries.empty() and has_battery) {
		//	if (fs::exists("/sys/class/power_supply")) {
		//		for (const auto& d : fs::directory_iterator("/sys/class/power_supply")) {
		//			//? Only consider online power supplies of type Battery or UPS
		//			//? see kernel docs for details on the file structure and contents
		//			//? https://www.kernel.org/doc/Documentation/ABI/testing/sysfs-class-power
		//			battery new_bat;
		//			fs::path bat_dir;
		//			try {
		//				if (not d.is_directory()
		//					or not fs::exists(d.path() / "type")
		//					or not fs::exists(d.path() / "present")
		//					or stoi(readfile(d.path() / "present")) != 1)
		//					continue;
		//				string dev_type = readfile(d.path() / "type");
		//				if (is_in(dev_type, "Battery", "UPS")) {
		//					bat_dir = d.path();
		//					new_bat.base_dir = d.path();
		//					new_bat.device_type = dev_type;
		//				}
		//			} catch (...) {
		//				//? skip power supplies not conforming to the kernel standard
		//				continue;
		//			}

		//			if (fs::exists(bat_dir / "energy_now")) new_bat.energy_now = bat_dir / "energy_now";
		//			else if (fs::exists(bat_dir / "charge_now")) new_bat.energy_now = bat_dir / "charge_now";
		//			else new_bat.use_energy = false;

		//			if (fs::exists(bat_dir / "energy_full")) new_bat.energy_full = bat_dir / "energy_full";
		//			else if (fs::exists(bat_dir / "charge_full")) new_bat.energy_full = bat_dir / "charge_full";
		//			else new_bat.use_energy = false;

		//			if (not new_bat.use_energy and not fs::exists(bat_dir / "capacity")) {
		//				continue;
		//			}

		//			if (fs::exists(bat_dir / "power_now")) new_bat.power_now = bat_dir / "power_now";
		//			else if (fs::exists(bat_dir / "current_now")) new_bat.power_now = bat_dir / "current_now";

		//			if (fs::exists(bat_dir / "AC0/online")) new_bat.online = bat_dir / "AC0/online";
		//			else if (fs::exists(bat_dir / "AC/online")) new_bat.online = bat_dir / "AC/online";

		//			batteries[bat_dir.filename()] = new_bat;
		//			Config::available_batteries.push_back(bat_dir.filename());
		//		}
		//	}
		//	if (batteries.empty()) {
		//		has_battery = false;
		//		return {0, 0, ""};
		//	}
		//}

		//auto& battery_sel = Config::getS("selected_battery");

		//if (auto_sel.empty()) {
		//	for (auto& [name, bat] : batteries) {
		//		if (bat.device_type == "Battery") {
		//			auto_sel = name;
		//			break;
		//		}
		//	}
		//	if (auto_sel.empty()) auto_sel = batteries.begin()->first;
		//}

		//auto& b = (battery_sel != "Auto" and batteries.contains(battery_sel) ? batteries.at(battery_sel) : batteries.at(auto_sel));

		//int percent = -1;
		//long seconds = -1;

		////? Try to get battery percentage
		//if (b.use_energy) {
		//	try {
		//		percent = round(100.0 * stoll(readfile(b.energy_now, "-1")) / stoll(readfile(b.energy_full, "1")));
		//	}
		//	catch (const std::invalid_argument&) { }
		//	catch (const std::out_of_range&) { }
		//}
		//if (percent < 0) {
		//	try {
		//		percent = stoll(readfile(b.base_dir / "capacity", "-1"));
		//	}
		//	catch (const std::invalid_argument&) { }
		//	catch (const std::out_of_range&) { }
		//}
		//if (percent < 0) {
		//	has_battery = false;
		//	return {0, 0, ""};
		//}

		////? Get charging/discharging status
		//string status = str_to_lower(readfile(b.base_dir / "status", "unknown"));
		//if (status == "unknown" and not b.online.empty()) {
		//	const auto online = readfile(b.online, "0");
		//	if (online == "1" and percent < 100) status = "charging";
		//	else if (online == "1") status = "full";
		//	else status = "discharging";
		//}

		////? Get seconds to empty
		//if (not is_in(status, "charging", "full")) {
		//	if (b.use_energy and not b.power_now.empty()) {
		//		try {
		//			seconds = round((double)stoll(readfile(b.energy_now, "0")) / stoll(readfile(b.power_now, "1")) * 3600);
		//		}
		//		catch (const std::invalid_argument&) { }
		//		catch (const std::out_of_range&) { }
		//	}
		//	if (seconds < 0 and fs::exists(b.base_dir / "time_to_empty")) {
		//		try {
		//			seconds = stoll(readfile(b.base_dir / "time_to_empty", "0")) * 60;
		//		}
		//		catch (const std::invalid_argument&) { }
		//		catch (const std::out_of_range&) { }
		//	}
		//}

		//return {percent, seconds, status};

		return { 0, 0, "" };
	}

	auto collect(const bool no_update) -> cpu_info& {
		if (Runner::stopping or (no_update and not current_cpu.cpu_percent.at("total").empty())) return current_cpu;
		auto& cpu = current_cpu;
	
		cpu.load_avg[0] = Cpu::load_avg_1m;
		cpu.load_avg[1] = Cpu::load_avg_5m;
		cpu.load_avg[2] = Cpu::load_avg_15m;

		vector<_SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> sppi(Shared::coreCount);
		if (not NT_SUCCESS(
				NtQuerySystemInformation(SystemProcessorPerformanceInformation,
				&sppi[0],
				Shared::coreCount * sizeof(_SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION),
				nullptr))){
			throw std::runtime_error("Failed to run Cpu::collect() -> NtQuerySystemInformation()");
		}

		vector<long long> idle, kernel, user, interrupt, dpc, total;
		long long totals;
		long long cpu_total = 0;

		//? Usage for each core
		for (int i = 0; i < Shared::coreCount; i++) {
			user.push_back(sppi[i].UserTime.QuadPart);
			idle.push_back(sppi[i].IdleTime.QuadPart);
			kernel.push_back(sppi[i].KernelTime.QuadPart - idle.back());
			dpc.push_back(sppi[i].Reserved1[0].QuadPart);
			interrupt.push_back(sppi[i].Reserved1[1].QuadPart);
			

			totals = 0;
			for (auto& v : { kernel, user, dpc, interrupt, idle }) totals += v.back();
			//for (auto& v : { idle, dpc, interrupt }) idles += v.back();

			const long long calc_totals = max(0ll, totals - core_old_totals.at(i));
			const long long calc_idles = max(0ll, idle.back() - core_old_idles.at(i));
			core_old_totals.at(i) = totals;
			core_old_idles.at(i) = idle.back();

			cpu.core_percent.at(i).push_back(clamp((long long)round((double)(calc_totals - calc_idles) * 100 / calc_totals), 0ll, 100ll));
			cpu_total += cpu.core_percent.at(i).back();

			//? Reduce size if there are more values than needed for graph
			if (cpu.core_percent.at(i).size() > 40) cpu.core_percent.at(i).pop_front();

		}

		//? Usage accumulated for total cpu usage
		vector<long long> times;
		totals = 0;
		for (auto& v : { kernel, user, dpc, interrupt, idle}) {
			times.push_back(std::accumulate(v.cbegin(), v.cend(), 0));
			totals += times.back();
		}
		//totals = times.at(0) + times.at(1) + times.at(2) + times.at(3);
		
		
		const long long calc_totals = max(1ll, totals - cpu_old.at("totals"));
		cpu_old.at("totals") = totals;

		//? Total usage of cpu
		cpu.cpu_percent.at("total").push_back(clamp(cpu_total / Shared::coreCount, 0ll, 100ll));

		//? Reduce size if there are more values than needed for graph
		while (cmp_greater(cpu.cpu_percent.at("total").size(), width * 2)) cpu.cpu_percent.at("total").pop_front();

		//? Populate cpu.cpu_percent with all fields from stat
		for (int ii = 0; const auto& val : times) {
			cpu.cpu_percent.at(time_names.at(ii)).push_back(clamp((long long)round((double)(val - cpu_old.at(time_names.at(ii))) * 100 / calc_totals), 0ll, 100ll));
			cpu_old.at(time_names.at(ii)) = val;

			//? Reduce size if there are more values than needed for graph
			while (cmp_greater(cpu.cpu_percent.at(time_names.at(ii)).size(), width * 2)) cpu.cpu_percent.at(time_names.at(ii)).pop_front();
			ii++;
		}

		if (Config::getB("show_cpu_freq"))
			cpuHz = get_cpuHz();

	//	if (Config::getB("check_temp") and got_sensors)
	//		update_sensors();

		has_battery = false;
		got_sensors = false;
	//	if (Config::getB("show_battery") and has_battery)
	//		current_bat = get_battery();

		return cpu;
	}
}

namespace Mem {
	bool has_swap = false;
	vector<string> fstab;
	fs::file_time_type fstab_time;
	int disk_ios = 0;
	vector<string> last_found;
	int64_t totalMem = 0;

	mem_info current_mem {};

	auto collect(const bool no_update) -> mem_info& {
		if (Runner::stopping or (no_update and not current_mem.percent.at("used").empty())) return current_mem;
		
		auto& show_swap = Config::getB("show_page");
		auto& swap_disk = Config::getB("page_disk");
		auto& show_disks = Config::getB("show_disks");
		auto& mem = current_mem;

		static MEMORYSTATUSEX memstat;
		memstat.dwLength = sizeof(memstat);
		
		if (GlobalMemoryStatusEx(&memstat) == 0) {
			throw std::runtime_error("Failed to run Mem::collect() -> GlobalMemoryStatusEx()");
		}

		totalMem = static_cast<int64_t>(memstat.ullTotalPhys);
		mem.stats.at("available") = static_cast<int64_t>(memstat.ullAvailPhys);
		mem.stats.at("used") = totalMem * memstat.dwMemoryLoad / 100;
		mem.stats.at("virtual") = static_cast<int64_t>(memstat.ullTotalVirtual) - static_cast<int64_t>(memstat.ullAvailVirtual);
		mem.stats.at("free") = totalMem - mem.stats.at("used");

		mem.stats.at("page_total") = static_cast<int64_t>(memstat.ullTotalPageFile) - totalMem;
		mem.stats.at("page_free") = static_cast<int64_t>(memstat.ullAvailPageFile);
		mem.stats.at("page_used") = mem.stats.at("page_total") - mem.stats.at("page_free");

		//? Calculate percentages
		for (const auto& name : mem_names) {
			mem.percent.at(name).push_back(round((double)mem.stats.at(name) * 100 / totalMem));
			while (cmp_greater(mem.percent.at(name).size(), width * 2)) mem.percent.at(name).pop_front();
		}

		if (show_swap and mem.stats.at("page_total") > 0) {
			for (const auto& name : swap_names) {
				mem.percent.at(name).push_back(round((double)mem.stats.at(name) * 100 / mem.stats.at("page_total")));
				while (cmp_greater(mem.percent.at(name).size(), width * 2)) mem.percent.at(name).pop_front();
			}
			has_swap = true;
		}
		else
			has_swap = false;

	//	//? Get disks stats
	//	if (show_disks) {
	//		double uptime = system_uptime();
	//		auto free_priv = Config::getB("disk_free_priv");
	//		try {
	//			auto& disks_filter = Config::getS("disks_filter");
	//			bool filter_exclude = false;
	//			auto& use_fstab = Config::getB("use_fstab");
	//			auto& only_physical = Config::getB("only_physical");
	//			auto& disks = mem.disks;
	//			ifstream diskread;

	//			vector<string> filter;
	//			if (not disks_filter.empty()) {
	//				filter = ssplit(disks_filter);
	//				if (filter.at(0).starts_with("exclude=")) {
	//					filter_exclude = true;
	//					filter.at(0) = filter.at(0).substr(8);
	//				}
	//			}

	//			//? Get list of "real" filesystems from /proc/filesystems
	//			vector<string> fstypes;
	//			if (only_physical and not use_fstab) {
	//				fstypes = {"zfs", "wslfs", "drvfs"};
	//				diskread.open(Shared::procPath / "filesystems");
	//				if (diskread.good()) {
	//					for (string fstype; diskread >> fstype;) {
	//						if (not is_in(fstype, "nodev", "squashfs", "nullfs"))
	//							fstypes.push_back(fstype);
	//						diskread.ignore(SSmax, '\n');
	//					}
	//				}
	//				else
	//					throw std::runtime_error("Failed to read /proc/filesystems");
	//				diskread.close();
	//			}

	//			//? Get disk list to use from fstab if enabled
	//			if (use_fstab and fs::last_write_time("/etc/fstab") != fstab_time) {
	//				fstab.clear();
	//				fstab_time = fs::last_write_time("/etc/fstab");
	//				diskread.open("/etc/fstab");
	//				if (diskread.good()) {
	//					for (string instr; diskread >> instr;) {
	//						if (not instr.starts_with('#')) {
	//							diskread >> instr;
	//							#ifdef SNAPPED
	//								if (instr == "/") fstab.push_back("/mnt");
	//								else if (not is_in(instr, "none", "swap")) fstab.push_back(instr);
	//							#else
	//								if (not is_in(instr, "none", "swap")) fstab.push_back(instr);
	//							#endif
	//						}
	//						diskread.ignore(SSmax, '\n');
	//					}
	//				}
	//				else
	//					throw std::runtime_error("Failed to read /etc/fstab");
	//				diskread.close();
	//			}

	//			//? Get mounts from /etc/mtab or /proc/self/mounts
	//			diskread.open((fs::exists("/etc/mtab") ? fs::path("/etc/mtab") : Shared::procPath / "self/mounts"));
	//			if (diskread.good()) {
	//				vector<string> found;
	//				found.reserve(last_found.size());
	//				string dev, mountpoint, fstype;
	//				while (not diskread.eof()) {
	//					std::error_code ec;
	//					diskread >> dev >> mountpoint >> fstype;
	//					diskread.ignore(SSmax, '\n');

	//					if (v_contains(found, mountpoint)) continue;

	//					//? Match filter if not empty
	//					if (not filter.empty()) {
	//						bool match = v_contains(filter, mountpoint);
	//						if ((filter_exclude and match) or (not filter_exclude and not match))
	//							continue;
	//					}

	//					if ((not use_fstab and not only_physical)
	//					or (use_fstab and v_contains(fstab, mountpoint))
	//					or (not use_fstab and only_physical and v_contains(fstypes, fstype))) {
	//						found.push_back(mountpoint);
	//						if (not v_contains(last_found, mountpoint)) redraw = true;

	//						//? Save mountpoint, name, dev path and path to /sys/block stat file
	//						if (not disks.contains(mountpoint)) {
	//							disks[mountpoint] = disk_info{fs::canonical(dev, ec), fs::path(mountpoint).filename()};
	//							if (disks.at(mountpoint).dev.empty()) disks.at(mountpoint).dev = dev;
	//							#ifdef SNAPPED
	//								if (mountpoint == "/mnt") disks.at(mountpoint).name = "root";
	//							#endif
	//							if (disks.at(mountpoint).name.empty()) disks.at(mountpoint).name = (mountpoint == "/" ? "root" : mountpoint);
	//							string devname = disks.at(mountpoint).dev.filename();
	//							while (devname.size() >= 2) {
	//								if (fs::exists("/sys/block/" + devname + "/stat", ec) and access(string("/sys/block/" + devname + "/stat").c_str(), R_OK) == 0) {
	//									disks.at(mountpoint).stat = "/sys/block/" + devname + "/stat";
	//									break;
	//								}
	//								devname.resize(devname.size() - 1);
	//							}
	//						}

	//					}
	//				}
	//				//? Remove disks no longer mounted or filtered out
	//				if (swap_disk and has_swap) found.push_back("swap");
	//				for (auto it = disks.begin(); it != disks.end();) {
	//					if (not v_contains(found, it->first))
	//						it = disks.erase(it);
	//					else
	//						it++;
	//				}
	//				if (found.size() != last_found.size()) redraw = true;
	//				last_found = std::move(found);
	//			}
	//			else
	//				throw std::runtime_error("Failed to get mounts from /etc/mtab and /proc/self/mounts");
	//			diskread.close();

	//			//? Get disk/partition stats
	//			for (auto& [mountpoint, disk] : disks) {
	//				if (std::error_code ec; not fs::exists(mountpoint, ec)) continue;
	//				struct statvfs64 vfs;
	//				if (statvfs64(mountpoint.c_str(), &vfs) < 0) {
	//					Logger::warning("Failed to get disk/partition stats with statvfs() for: " + mountpoint);
	//					continue;
	//				}
	//				disk.total = vfs.f_blocks * vfs.f_frsize;
	//				disk.free = (free_priv ? vfs.f_bfree : vfs.f_bavail) * vfs.f_frsize;
	//				disk.used = disk.total - disk.free;
	//				disk.used_percent = round((double)disk.used * 100 / disk.total);
	//				disk.free_percent = 100 - disk.used_percent;
	//			}

	//			//? Setup disks order in UI and add swap if enabled
	//			mem.disks_order.clear();
	//			#ifdef SNAPPED
	//				if (disks.contains("/mnt")) mem.disks_order.push_back("/mnt");
	//			#else
	//				if (disks.contains("/")) mem.disks_order.push_back("/");
	//			#endif
	//			if (swap_disk and has_swap) {
	//				mem.disks_order.push_back("swap");
	//				if (not disks.contains("swap")) disks["swap"] = {"", "swap"};
	//				disks.at("swap").total = mem.stats.at("swap_total");
	//				disks.at("swap").used = mem.stats.at("swap_used");
	//				disks.at("swap").free = mem.stats.at("swap_free");
	//				disks.at("swap").used_percent = mem.percent.at("swap_used").back();
	//				disks.at("swap").free_percent = mem.percent.at("swap_free").back();
	//			}
	//			for (const auto& name : last_found)
	//				#ifdef SNAPPED
	//					if (not is_in(name, "/mnt", "swap")) mem.disks_order.push_back(name);
	//				#else
	//					if (not is_in(name, "/", "swap")) mem.disks_order.push_back(name);
	//				#endif

	//			//? Get disks IO
	//			int64_t sectors_read, sectors_write, io_ticks;
	//			disk_ios = 0;
	//			for (auto& [ignored, disk] : disks) {
	//				if (disk.stat.empty() or access(disk.stat.c_str(), R_OK) != 0) continue;
	//				diskread.open(disk.stat);
	//				if (diskread.good()) {
	//					disk_ios++;
	//					for (int i = 0; i < 2; i++) { diskread >> std::ws; diskread.ignore(SSmax, ' '); }
	//					diskread >> sectors_read;
	//					if (disk.io_read.empty())
	//						disk.io_read.push_back(0);
	//					else
	//						disk.io_read.push_back(max((int64_t)0, (sectors_read - disk.old_io.at(0)) * 512));
	//					disk.old_io.at(0) = sectors_read;
	//					while (cmp_greater(disk.io_read.size(), width * 2)) disk.io_read.pop_front();

	//					for (int i = 0; i < 3; i++) { diskread >> std::ws; diskread.ignore(SSmax, ' '); }
	//					diskread >> sectors_write;
	//					if (disk.io_write.empty())
	//						disk.io_write.push_back(0);
	//					else
	//						disk.io_write.push_back(max((int64_t)0, (sectors_write - disk.old_io.at(1)) * 512));
	//					disk.old_io.at(1) = sectors_write;
	//					while (cmp_greater(disk.io_write.size(), width * 2)) disk.io_write.pop_front();

	//					for (int i = 0; i < 2; i++) { diskread >> std::ws; diskread.ignore(SSmax, ' '); }
	//					diskread >> io_ticks;
	//					if (disk.io_activity.empty())
	//						disk.io_activity.push_back(0);
	//					else
	//						disk.io_activity.push_back(clamp((long)round((double)(io_ticks - disk.old_io.at(2)) / (uptime - old_uptime) / 10), 0l, 100l));
	//					disk.old_io.at(2) = io_ticks;
	//					while (cmp_greater(disk.io_activity.size(), width * 2)) disk.io_activity.pop_front();
	//				}
	//				diskread.close();
	//			}
	//			old_uptime = uptime;
	//		}
	//		catch (const std::exception& e) {
	//			Logger::warning("Error in Mem::collect() : " + (string)e.what());
	//		}
	//	}

	//	return mem;
	return current_mem;
	}

}

namespace Net {
	unordered_flat_map<string, net_info> current_net;
	net_info empty_net = {};
	vector<string> interfaces;
	string selected_iface;
	int errors = 0;
	unordered_flat_map<string, uint64_t> graph_max = { {"download", {}}, {"upload", {}} };
	unordered_flat_map<string, array<int, 2>> max_count = { {"download", {}}, {"upload", {}} };
	bool rescale = true;
	uint64_t timestamp = 0;

	//* RAII wrapper for getifaddrs
	//class getifaddr_wrapper {
	//	struct ifaddrs* ifaddr;
	//public:
	//	int status;
	//	getifaddr_wrapper() { status = getifaddrs(&ifaddr); }
	//	~getifaddr_wrapper() { freeifaddrs(ifaddr); }
	//	auto operator()() -> struct ifaddrs* { return ifaddr; }
	//};

	auto collect(const bool no_update) -> net_info& {
		auto& net = current_net;

		return empty_net;

	//	auto& config_iface = Config::getS("net_iface");
	//	auto& net_sync = Config::getB("net_sync");
	//	auto& net_auto = Config::getB("net_auto");
	//	auto new_timestamp = time_ms();

	//	if (not no_update and errors < 3) {
	//		//? Get interface list using getifaddrs() wrapper
	//		getifaddr_wrapper if_wrap {};
	//		if (if_wrap.status != 0) {
	//			errors++;
	//			Logger::error("Net::collect() -> getifaddrs() failed with id " + to_string(if_wrap.status));
	//			redraw = true;
	//			return empty_net;
	//		}
	//		int family = 0;
	//		char ip[NI_MAXHOST];
	//		interfaces.clear();
	//		string ipv4, ipv6;

	//		//? Iteration over all items in getifaddrs() list
	//		for (auto* ifa = if_wrap(); ifa != NULL; ifa = ifa->ifa_next) {
	//			if (ifa->ifa_addr == NULL) continue;
	//			family = ifa->ifa_addr->sa_family;
	//			const auto& iface = ifa->ifa_name;

	//			//? Get IPv4 address
	//			if (family == AF_INET) {
	//				if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), ip, NI_MAXHOST, NULL, 0, NI_NUMERICHOST) == 0)
	//					net[iface].ipv4 = ip;
	//			}
	//			//? Get IPv6 address
	//			else if (family == AF_INET6) {
	//				if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in6), ip, NI_MAXHOST, NULL, 0, NI_NUMERICHOST) == 0)
	//					net[iface].ipv6 = ip;
	//			}

	//			//? Update available interfaces vector and get status of interface
	//			if (not v_contains(interfaces, iface)) {
	//				interfaces.push_back(iface);
	//				net[iface].connected = (ifa->ifa_flags & IFF_RUNNING);
	//			}
	//		}

	//		//? Get total recieved and transmitted bytes + device address if no ip was found
	//		for (const auto& iface : interfaces) {
	//			if (net.at(iface).ipv4.empty() and net.at(iface).ipv6.empty())
	//				net.at(iface).ipv4 = readfile("/sys/class/net/" + iface + "/address");

	//			for (const string dir : {"download", "upload"}) {
	//				const fs::path sys_file = "/sys/class/net/" + iface + "/statistics/" + (dir == "download" ? "rx_bytes" : "tx_bytes");
	//				auto& saved_stat = net.at(iface).stat.at(dir);
	//				auto& bandwidth = net.at(iface).bandwidth.at(dir);

	//				uint64_t val = 0;
	//				try { val = (uint64_t)stoull(readfile(sys_file, "0")); }
	//				catch (const std::invalid_argument&) {}
	//				catch (const std::out_of_range&) {}

	//				//? Update speed, total and top values
	//				if (val < saved_stat.last) {
	//					saved_stat.rollover += saved_stat.last;
	//					saved_stat.last = 0;
	//				}
	//				if (cmp_greater((unsigned long long)saved_stat.rollover + (unsigned long long)val, numeric_limits<uint64_t>::max())) {
	//					saved_stat.rollover = 0;
	//					saved_stat.last = 0;
	//				}
	//				saved_stat.speed = round((double)(val - saved_stat.last) / ((double)(new_timestamp - timestamp) / 1000));
	//				if (saved_stat.speed > saved_stat.top) saved_stat.top = saved_stat.speed;
	//				if (saved_stat.offset > val + saved_stat.rollover) saved_stat.offset = 0;
	//				saved_stat.total = (val + saved_stat.rollover) - saved_stat.offset;
	//				saved_stat.last = val;

	//				//? Add values to graph
	//				bandwidth.push_back(saved_stat.speed);
	//				while (cmp_greater(bandwidth.size(), width * 2)) bandwidth.pop_front();

	//				//? Set counters for auto scaling
	//				if (net_auto and selected_iface == iface) {
	//					if (saved_stat.speed > graph_max[dir]) {
	//						++max_count[dir][0];
	//						if (max_count[dir][1] > 0) --max_count[dir][1];
	//					}
	//					else if (graph_max[dir] > 10 << 10 and saved_stat.speed < graph_max[dir] / 10) {
	//						++max_count[dir][1];
	//						if (max_count[dir][0] > 0) --max_count[dir][0];
	//					}

	//				}
	//			}
	//		}

	//		//? Clean up net map if needed
	//		if (net.size() > interfaces.size()) {
	//			for (auto it = net.begin(); it != net.end();) {
	//				if (not v_contains(interfaces, it->first))
	//					it = net.erase(it);
	//				else
	//					it++;
	//			}
	//			net.compact();
	//		}

	//		timestamp = new_timestamp;
	//	}

	//	//? Return empty net_info struct if no interfaces was found
	//	if (net.empty())
	//		return empty_net;

	//	//? Find an interface to display if selected isn't set or valid
	//	if (selected_iface.empty() or not v_contains(interfaces, selected_iface)) {
	//		max_count["download"][0] = max_count["download"][1] = max_count["upload"][0] = max_count["upload"][1] = 0;
	//		redraw = true;
	//		if (net_auto) rescale = true;
	//		if (not config_iface.empty() and v_contains(interfaces, config_iface)) selected_iface = config_iface;
	//		else {
	//			//? Sort interfaces by total upload + download bytes
	//			auto sorted_interfaces = interfaces;
	//			rng::sort(sorted_interfaces, [&](const auto& a, const auto& b){
	//				return 	cmp_greater(net.at(a).stat["download"].total + net.at(a).stat["upload"].total,
	//									net.at(b).stat["download"].total + net.at(b).stat["upload"].total);
	//			});
	//			selected_iface.clear();
	//			//? Try to set to a connected interface
	//			for (const auto& iface : sorted_interfaces) {
	//				if (net.at(iface).connected) selected_iface = iface;
	//				break;
	//			}
	//			//? If no interface is connected set to first available
	//			if (selected_iface.empty() and not sorted_interfaces.empty()) selected_iface = sorted_interfaces.at(0);
	//			else if (sorted_interfaces.empty()) return empty_net;

	//		}
	//	}

	//	//? Calculate max scale for graphs if needed
	//	if (net_auto) {
	//		bool sync = false;
	//		for (const auto& dir: {"download", "upload"}) {
	//			for (const auto& sel : {0, 1}) {
	//				if (rescale or max_count[dir][sel] >= 5) {
	//					const uint64_t avg_speed = (net[selected_iface].bandwidth[dir].size() > 5
	//						? std::accumulate(net.at(selected_iface).bandwidth.at(dir).rbegin(), net.at(selected_iface).bandwidth.at(dir).rbegin() + 5, 0) / 5
	//						: net[selected_iface].stat[dir].speed);
	//					graph_max[dir] = max(uint64_t(avg_speed * (sel == 0 ? 1.3 : 3.0)), (uint64_t)10 << 10);
	//					max_count[dir][0] = max_count[dir][1] = 0;
	//					redraw = true;
	//					if (net_sync) sync = true;
	//					break;
	//				}
	//			}
	//			//? Sync download/upload graphs if enabled
	//			if (sync) {
	//				const auto other = (string(dir) == "upload" ? "download" : "upload");
	//				graph_max[other] = graph_max[dir];
	//				max_count[other][0] = max_count[other][1] = 0;
	//				break;
	//			}
	//		}
	//	}

	//	rescale = false;
	//	return net.at(selected_iface);
	}
}

namespace Proc {

	vector<proc_info> current_procs;
	unordered_flat_map<string, string> uid_user;
	string current_sort;
	string current_filter;
	bool current_rev = false;

	fs::file_time_type passwd_time;

	uint64_t cputimes;
	int collapse = -1, expand = -1;
	uint64_t old_cputimes = 0;
	atomic<int> numpids = 0;
	int filter_found = 0;

	detail_container detailed;

	//* Generate process tree list
	void _tree_gen(proc_info& cur_proc, vector<proc_info>& in_procs, vector<std::reference_wrapper<proc_info>>& out_procs, int cur_depth, const bool collapsed, const string& filter, bool found=false, const bool no_update=false, const bool should_filter=false) {
		auto cur_pos = out_procs.size();
		bool filtering = false;

		//? If filtering, include children of matching processes
		if (not found and (should_filter or not filter.empty())) {
			if (not s_contains(std::to_string(cur_proc.pid), filter)
			and not s_contains(cur_proc.name, filter)
			and not s_contains(cur_proc.cmd, filter)
			and not s_contains(cur_proc.user, filter)) {
				filtering = true;
				cur_proc.filtered = true;
				filter_found++;
			}
			else {
				found = true;
				cur_depth = 0;
			}
		}
		else if (cur_proc.filtered) cur_proc.filtered = false;

		//? Set tree index position for process if not filtered out or currently in a collapsed sub-tree
		if (not collapsed and not filtering) {
			out_procs.push_back(std::ref(cur_proc));
			cur_proc.tree_index = out_procs.size() - 1;
			//? Try to find name of the binary file and append to program name if not the same
			if (cur_proc.short_cmd.empty() and not cur_proc.cmd.empty()) {
				std::string_view cmd_view = cur_proc.cmd;
				cmd_view = cmd_view.substr((size_t)0, min(cmd_view.find(' '), cmd_view.size()));
				cmd_view = cmd_view.substr(min(cmd_view.find_last_of('/') + 1, cmd_view.size()));
				cur_proc.short_cmd = (string)cmd_view;
			}
		}
		else {
			cur_proc.tree_index = in_procs.size();
		}

		//? Recursive iteration over all children
		int children = 0;
		for (auto& p : rng::equal_range(in_procs, cur_proc.pid, rng::less{}, &proc_info::ppid)) {
			if (not no_update and not filtering and (collapsed or cur_proc.collapsed)) {
				out_procs.back().get().cpu_p += p.cpu_p;
				out_procs.back().get().mem += p.mem;
				out_procs.back().get().threads += p.threads;
				filter_found++;
			}
			if (collapsed and not filtering) {
				cur_proc.filtered = true;
			}
			else children++;
			_tree_gen(p, in_procs, out_procs, cur_depth + 1, (collapsed ? true : cur_proc.collapsed), filter, found, no_update, should_filter);
		}
		if (collapsed or filtering) return;

		//? Add tree terminator symbol if it's the last child in a sub-tree
		if (out_procs.size() > cur_pos + 1 and not out_procs.back().get().prefix.ends_with("]─"))
			out_procs.back().get().prefix.replace(out_procs.back().get().prefix.size() - 8, 8, " └─ ");

		//? Add collapse/expand symbols if process have any children
		out_procs.at(cur_pos).get().prefix = " │ "s * cur_depth + (children > 0 ? (cur_proc.collapsed ? "[+]─" : "[-]─") : " ├─ ");
	}

	//* Get detailed info for selected process
	void _collect_details(const size_t pid, const uint64_t uptime, vector<proc_info>& procs) {
		return;
		//fs::path pid_path = Shared::procPath / std::to_string(pid);

		//if (pid != detailed.last_pid) {
		//	detailed = {};
		//	detailed.last_pid = pid;
		//	detailed.skip_smaps = not Config::getB("proc_info_smaps");
		//}

		////? Copy proc_info for process from proc vector
		//auto p_info = rng::find(procs, pid, &proc_info::pid);
		//detailed.entry = *p_info;

		////? Update cpu percent deque for process cpu graph
		//if (not Config::getB("proc_per_core")) detailed.entry.cpu_p *= Shared::coreCount;
		//detailed.cpu_percent.push_back(clamp((long long)round(detailed.entry.cpu_p), 0ll, 100ll));
		//while (cmp_greater(detailed.cpu_percent.size(), width)) detailed.cpu_percent.pop_front();

		////? Process runtime
		//detailed.elapsed = sec_to_dhms(uptime - (detailed.entry.cpu_s / Shared::clkTck));
		//if (detailed.elapsed.size() > 8) detailed.elapsed.resize(detailed.elapsed.size() - 3);

		////? Get parent process name
		//if (detailed.parent.empty()) {
		//	auto p_entry = rng::find(procs, detailed.entry.ppid, &proc_info::pid);
		//	if (p_entry != procs.end()) detailed.parent = p_entry->name;
		//}

		////? Expand process status from single char to explanative string
		//detailed.status = (proc_states.contains(detailed.entry.state)) ? proc_states.at(detailed.entry.state) : "Unknown";

		//ifstream d_read;
		//string short_str;

		////? Try to get RSS mem from proc/[pid]/smaps
		//detailed.memory.clear();
		//if (not detailed.skip_smaps and fs::exists(pid_path / "smaps")) {
		//	d_read.open(pid_path / "smaps");
		//	uint64_t rss = 0;
		//	try {
		//		while (d_read.good()) {
		//			d_read.ignore(SSmax, 'R');
		//			if (d_read.peek() == 's') {
		//				d_read.ignore(SSmax, ':');
		//				getline(d_read, short_str, 'k');
		//				rss += stoull(short_str);
		//			}
		//		}
		//		if (rss == detailed.entry.mem >> 10)
		//			detailed.skip_smaps = true;
		//		else {
		//			detailed.mem_bytes.push_back(rss << 10);
		//			detailed.memory = floating_humanizer(rss, false, 1);
		//		}
		//	}
		//	catch (const std::invalid_argument&) {}
		//	catch (const std::out_of_range&) {}
		//	d_read.close();
		//}
		//if (detailed.memory.empty()) {
		//	detailed.mem_bytes.push_back(detailed.entry.mem);
		//	detailed.memory = floating_humanizer(detailed.entry.mem);
		//}
		//if (detailed.first_mem == -1 or detailed.first_mem < detailed.mem_bytes.back() / 2 or detailed.first_mem > detailed.mem_bytes.back() * 4) {
		//	detailed.first_mem = min((uint64_t)detailed.mem_bytes.back() * 2, Mem::get_totalMem());
		//	redraw = true;
		//}

		//while (cmp_greater(detailed.mem_bytes.size(), width)) detailed.mem_bytes.pop_front();

		////? Get bytes read and written from proc/[pid]/io
		//if (fs::exists(pid_path / "io")) {
		//	d_read.open(pid_path / "io");
		//	try {
		//		string name;
		//		while (d_read.good()) {
		//			getline(d_read, name, ':');
		//			if (name.ends_with("read_bytes")) {
		//				getline(d_read, short_str);
		//				detailed.io_read = floating_humanizer(stoull(short_str));
		//			}
		//			else if (name.ends_with("write_bytes")) {
		//				getline(d_read, short_str);
		//				detailed.io_write = floating_humanizer(stoull(short_str));
		//				break;
		//			}
		//			else
		//				d_read.ignore(SSmax, '\n');
		//		}
		//	}
		//	catch (const std::invalid_argument&) {}
		//	catch (const std::out_of_range&) {}
		//	d_read.close();
		//}
	}

	//* Collects and sorts process information from /proc
	auto collect(const bool no_update) -> vector<proc_info>& {
		const auto& sorting = Config::getS("proc_sorting");
		const auto& reverse = Config::getB("proc_reversed");
		const auto& filter = Config::getS("proc_filter");
		const auto& per_core = Config::getB("proc_per_core");
		const auto& tree = Config::getB("proc_tree");
		const auto& show_detailed = Config::getB("show_detailed");
		const size_t detailed_pid = Config::getI("detailed_pid");
		bool should_filter = current_filter != filter;
		if (should_filter) current_filter = filter;
		const bool sorted_change = (sorting != current_sort or reverse != current_rev or should_filter);
		if (sorted_change) {
			current_sort = sorting;
			current_rev = reverse;
		}

		const double uptime = system_uptime();

		const int cmult = (per_core) ? Shared::coreCount : 1;
		bool got_detailed = false;

		//* Use pids from last update if only changing filter, sorting or tree options
		if (no_update and not current_procs.empty()) {
			if (show_detailed and detailed_pid != detailed.last_pid) _collect_details(detailed_pid, round(uptime), current_procs);
		}
		//* ---------------------------------------------Collection start----------------------------------------------
		else {
			should_filter = true;

			auto totalMem = Mem::totalMem;

	//		int totalMem_len = to_string(totalMem >> 10).size();

	//		//? Update uid_user map if /etc/passwd changed since last run
	//		if (not Shared::passwd_path.empty() and fs::last_write_time(Shared::passwd_path) != passwd_time) {
	//			string r_uid, r_user;
	//			passwd_time = fs::last_write_time(Shared::passwd_path);
	//			uid_user.clear();
	//			pread.open(Shared::passwd_path);
	//			if (pread.good()) {
	//				while (pread.good()) {
	//					getline(pread, r_user, ':');
	//					pread.ignore(SSmax, ':');
	//					getline(pread, r_uid, ':');
	//					uid_user[r_uid] = r_user;
	//					pread.ignore(SSmax, '\n');
	//				}
	//			}
	//			else {
	//				Shared::passwd_path.clear();
	//			}
	//			pread.close();
	//		}

	//		//? Get cpu total times from /proc/stat
	//		cputimes = 0;
	//		pread.open(Shared::procPath / "stat");
	//		if (pread.good()) {
	//			pread.ignore(SSmax, ' ');
	//			for (uint64_t times; pread >> times; cputimes += times);
	//		}
	//		else throw std::runtime_error("Failure to read /proc/stat");
	//		pread.close();

			//? Iterate over all pids in /proc
			vector<size_t> found;		
			HandleWrapper pSnap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));

			if (not pSnap.valid) {
				Logger::warning("Proc::collect() -> CreateToolhelp32Snapshot() failed!");
				return current_procs;
			}

			PROCESSENTRY32 pe;
			pe.dwSize = sizeof(PROCESSENTRY32);

			if (not Process32First(pSnap(), &pe)) {
				Logger::warning("Proc::collect() -> Process32First() failed!");
				return current_procs;
			}

			do {
				
				size_t pid = pe.th32ProcessID;
				if (pid == 0) continue;

				HandleWrapper pHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID));
				if (not pHandle.valid) continue;
			
				if (Runner::stopping)
					return current_procs;
				
				found.push_back(pid);

				//? Check if pid already exists in current_procs
				auto find_old = rng::find(current_procs, pid, &proc_info::pid);
				bool no_cache = false;
				if (find_old == current_procs.end()) {
					current_procs.push_back({pid});
					find_old = current_procs.end() - 1;
					no_cache = true;
				}

				auto& new_proc = *find_old;

				//? Get program name, command and username
				if (no_cache) {
					new_proc.name = CW2A(pe.szExeFile);
					
					new_proc.cmd = new_proc.name;
					if (new_proc.cmd.size() > 1000) {
						new_proc.cmd.resize(1000);
						break;
					}

					new_proc.name = new_proc.name.substr(0, new_proc.name.find_last_of('.'));
					
					/*SID_NAME_USE peUse;
					DWORD cchName = 0;
					DWORD cchRefDomain = 0;

					new_proc.user = "unknown";
					
					LookupAccountSidW(nullptr, process.pUserSid, nullptr, &cchName, nullptr, &cchRefDomain, &peUse);
					
					vector<wchar_t> Name(cchName + 1);
					vector<wchar_t> RefDomain(cchRefDomain + 1);
					if (LookupAccountSidW(nullptr, &process.pUserSid, Name.data(), &cchName, RefDomain.data(), &cchRefDomain, &peUse)) {
						new_proc.user = string(CW2A(Name.data()));
					}
					else {
						new_proc.user = to_string(GetLastError());
					}*/
				}

				new_proc.mem = 10;

				new_proc.threads = pe.cntThreads;
				new_proc.ppid = pe.th32ParentProcessID;
				

	//			//? Parse /proc/[pid]/stat
	//			pread.open(d.path() / "stat");
	//			if (not pread.good()) continue;

	//			const auto& offset = new_proc.name_offset;
	//			short_str.clear();
	//			int x = 0, next_x = 3;
	//			uint64_t cpu_t = 0;
	//			try {
	//				for (;;) {
	//					while (pread.good() and ++x < next_x + offset) pread.ignore(SSmax, ' ');
	//					if (not pread.good()) break;
	//					else getline(pread, short_str, ' ');

	//					switch (x-offset) {
	//						case 3: //? Process state
	//							new_proc.state = short_str.at(0);
	//							if (new_proc.ppid != 0) next_x = 14;
	//							continue;
	//						case 4: //? Parent pid
	//							new_proc.ppid = stoull(short_str);
	//							next_x = 14;
	//							continue;
	//						case 14: //? Process utime
	//							cpu_t = stoull(short_str);
	//							continue;
	//						case 15: //? Process stime
	//							cpu_t += stoull(short_str);
	//							next_x = 19;
	//							continue;
	//						case 19: //? Nice value
	//							new_proc.p_nice = stoull(short_str);
	//							continue;
	//						case 20: //? Number of threads
	//							new_proc.threads = stoull(short_str);
	//							if (new_proc.cpu_s == 0) {
	//								next_x = 22;
	//								new_proc.cpu_t = cpu_t;
	//							}
	//							else
	//								next_x = 24;
	//							continue;
	//						case 22: //? Get cpu seconds if missing
	//							new_proc.cpu_s = stoull(short_str);
	//							next_x = 24;
	//							continue;
	//						case 24: //? RSS memory (can be inaccurate, but parsing smaps increases total cpu usage by ~20x)
	//							if (cmp_greater(short_str.size(), totalMem_len))
	//								new_proc.mem = totalMem;
	//							else
	//								new_proc.mem = stoull(short_str) * Shared::pageSize;
	//					}
	//					break;
	//				}

	//			}
	//			catch (const std::invalid_argument&) { continue; }
	//			catch (const std::out_of_range&) { continue; }

	//			pread.close();

	//			if (x-offset < 24) continue;

	//			//? Get RSS memory from /proc/[pid]/statm if value from /proc/[pid]/stat looks wrong
	//			if (new_proc.mem >= totalMem) {
	//				pread.open(d.path() / "statm");
	//				if (not pread.good()) continue;
	//				pread.ignore(SSmax, ' ');
	//				pread >> new_proc.mem;
	//				new_proc.mem *= Shared::pageSize;
	//				pread.close();
	//			}

	//			//? Process cpu usage since last update
	//			new_proc.cpu_p = clamp(round(cmult * 1000 * (cpu_t - new_proc.cpu_t) / max((uint64_t)1, cputimes - old_cputimes)) / 10.0, 0.0, 100.0 * Shared::coreCount);

	//			//? Process cumulative cpu usage since process start
	//			new_proc.cpu_c = (double)cpu_t / max(1.0, (uptime * Shared::clkTck) - new_proc.cpu_s);

	//			//? Update cached value with latest cpu times
	//			new_proc.cpu_t = cpu_t;

	//			if (show_detailed and not got_detailed and new_proc.pid == detailed_pid) {
	//				got_detailed = true;
	//			}
			} while (Process32Next(pSnap(), &pe));

			//? Clear dead processes from current_procs
			auto eraser = rng::remove_if(current_procs, [&](const auto& element){ return not v_contains(found, element.pid); });
			current_procs.erase(eraser.begin(), eraser.end());

	//		//? Update the details info box for process if active
	//		if (show_detailed and got_detailed) {
	//			_collect_details(detailed_pid, round(uptime), current_procs);
	//		}
	//		else if (show_detailed and not got_detailed and detailed.status != "Dead") {
	//			detailed.status = "Dead";
	//			redraw = true;
			//}

	//		old_cputimes = cputimes;
		}
		//* ---------------------------------------------Collection done-----------------------------------------------

		//* Sort processes
		if (sorted_change or not no_update) {
			if (reverse) {
				switch (v_index(sort_vector, sorting)) {
					case 0: rng::stable_sort(current_procs, rng::less{}, &proc_info::pid); 		break;
					case 1: rng::stable_sort(current_procs, rng::less{}, &proc_info::name);		break;
					case 2: rng::stable_sort(current_procs, rng::less{}, &proc_info::cmd); 		break;
					case 3: rng::stable_sort(current_procs, rng::less{}, &proc_info::threads); 	break;
					case 4: rng::stable_sort(current_procs, rng::less{}, &proc_info::user); 	break;
					case 5: rng::stable_sort(current_procs, rng::less{}, &proc_info::mem); 		break;
					case 6: rng::stable_sort(current_procs, rng::less{}, &proc_info::cpu_p);   	break;
					case 7: rng::stable_sort(current_procs, rng::less{}, &proc_info::cpu_c);   	break;
				}
			} else {
				switch (v_index(sort_vector, sorting)) {
						case 0: rng::stable_sort(current_procs, rng::greater{}, &proc_info::pid); 		break;
						case 1: rng::stable_sort(current_procs, rng::greater{}, &proc_info::name);		break;
						case 2: rng::stable_sort(current_procs, rng::greater{}, &proc_info::cmd); 		break;
						case 3: rng::stable_sort(current_procs, rng::greater{}, &proc_info::threads); 	break;
						case 4: rng::stable_sort(current_procs, rng::greater{}, &proc_info::user); 	break;
						case 5: rng::stable_sort(current_procs, rng::greater{}, &proc_info::mem); 		break;
						case 6: rng::stable_sort(current_procs, rng::greater{}, &proc_info::cpu_p);   	break;
						case 7: rng::stable_sort(current_procs, rng::greater{}, &proc_info::cpu_c);   	break;
				}
			}

			//* When sorting with "cpu lazy" push processes over threshold cpu usage to the front regardless of cumulative usage
			if (not tree and not reverse and sorting == "cpu lazy") {
				double max = 10.0, target = 30.0;
				for (size_t i = 0, x = 0, offset = 0; i < current_procs.size(); i++) {
					if (i <= 5 and current_procs.at(i).cpu_p > max)
						max = current_procs.at(i).cpu_p;
					else if (i == 6)
						target = (max > 30.0) ? max : 10.0;
					if (i == offset and current_procs.at(i).cpu_p > 30.0)
						offset++;
					else if (current_procs.at(i).cpu_p > target) {
						rotate(current_procs.begin() + offset, current_procs.begin() + i, current_procs.begin() + i + 1);
						if (++x > 10) break;
					}
				}
			}
		}

		//* Match filter if defined
		if (should_filter) {
			filter_found = 0;
			for (auto& p : current_procs) {
				if (not tree and not filter.empty()) {
						if (not s_contains(to_string(p.pid), filter)
						and not s_contains(p.name, filter)
						and not s_contains(p.cmd, filter)
						and not s_contains(p.user, filter)) {
							p.filtered = true;
							filter_found++;
							}
						else {
							p.filtered = false;
						}
					}
				else {
					p.filtered = false;
				}
			}
		}

		//* Generate tree view if enabled
		//if (tree and (not no_update or should_filter or sorted_change)) {
		//	if (auto find_pid = (collapse != -1 ? collapse : expand); find_pid != -1) {
		//		auto collapser = rng::find(current_procs, find_pid, &proc_info::pid);
		//		if (collapser != current_procs.end()) {
		//			if (collapse == expand) {
		//				collapser->collapsed = not collapser->collapsed;
		//			}
		//			else if (collapse > -1) {
		//				collapser->collapsed = true;
		//			}
		//			else if (expand > -1) {
		//				collapser->collapsed = false;
		//			}
		//		}
		//		collapse = expand = -1;
		//	}
		//	if (should_filter or not filter.empty()) filter_found = 0;

		//	vector<std::reference_wrapper<proc_info>> tree_procs;
		//	tree_procs.reserve(current_procs.size());

		//	//? Stable sort to retain selected sorting among processes with the same parent
		//	rng::stable_sort(current_procs, rng::less{}, &proc_info::ppid);

		//	//? Start recursive iteration over processes with the lowest shared parent pids
		//	for (auto& p : rng::equal_range(current_procs, current_procs.at(0).ppid, rng::less{}, &proc_info::ppid)) {
		//		_tree_gen(p, current_procs, tree_procs, 0, false, filter, false, no_update, should_filter);
		//	}

		//	//? Final sort based on tree index
		//	rng::stable_sort(current_procs, rng::less{}, &proc_info::tree_index);
		//}

		numpids = (int)current_procs.size() - filter_found;

		return current_procs;
	}
}

namespace Tools {
	double system_uptime() {
		return 0.0;
		//string upstr;
		//ifstream pread(Shared::procPath / "uptime");
		//if (pread.good()) {
		//	try {
		//		getline(pread, upstr, ' ');
		//		pread.close();
		//		return stod(upstr);
		//	}
		//	catch (const std::invalid_argument&) {}
		//	catch (const std::out_of_range&) {}
		//}
		//throw std::runtime_error("Failed get uptime from from " + (string)Shared::procPath + "/uptime");
	}
}
