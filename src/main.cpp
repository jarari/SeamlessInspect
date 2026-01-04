#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace RE;
using std::unordered_map;

template <class Ty>
Ty SafeWrite64Function(uintptr_t addr, Ty data)
{
	DWORD oldProtect;
	void* _d[2];
	memcpy(_d, &data, sizeof(data));
	size_t len = sizeof(_d[0]);

	VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect);
	Ty olddata;
	memset(&olddata, 0, sizeof(Ty));
	memcpy(&olddata, (void*)addr, len);
	memcpy((void*)addr, &_d[0], len);
	VirtualProtect((void*)addr, len, oldProtect, &oldProtect);
	return olddata;
}

namespace
{
	constexpr char kConfigFileName[] = "SeamlessInspectMods.txt";
	constexpr char kDefaultPluginName[] = "Inspectweapons.esl";
	constexpr uintptr_t kInspectQuestFormID = 0x805;

	std::vector<std::string> g_pluginFilenames;
	bool g_warnedMissingQuest = false;

	std::string TrimLine(std::string_view a_line)
	{
		const auto first = a_line.find_first_not_of(" \t\r\n");
		if (first == std::string_view::npos) {
			return {};
		}

		const auto last = a_line.find_last_not_of(" \t\r\n");
		return std::string{ a_line.substr(first, last - first + 1) };
	}

	std::filesystem::path GetConfigPath()
	{
		wchar_t runtimePath[MAX_PATH];
		const auto length = ::GetModuleFileNameW(nullptr, runtimePath, MAX_PATH);
		if (length == 0 || length == MAX_PATH) {
			logger::warn("Failed to resolve runtime path for SeamlessInspect config lookup");
			return {};
		}

		std::filesystem::path path(runtimePath);
		path = path.parent_path() / "Data" / "F4SE" / "Plugins" / kConfigFileName;
		return path;
	}

	void LoadPluginFilenames()
	{
		g_pluginFilenames.clear();
		const auto configPath = GetConfigPath();
		if (!configPath.empty()) {
			std::ifstream stream(configPath);
			if (stream.is_open()) {
				std::string line;
				while (std::getline(stream, line)) {
					auto trimmed = TrimLine(line);
					if (!trimmed.empty()) {
						g_pluginFilenames.emplace_back(std::move(trimmed));
					}
				}

				if (g_pluginFilenames.empty()) {
					logger::warn("Config file {} was empty; SeamlessInspect will use default plugin", configPath.string());
				} else {
					logger::info("Loaded {} plugin filename(s) from {}", g_pluginFilenames.size(), configPath.string());
				}
			} else {
				logger::warn("Failed to open config file {}; SeamlessInspect will use default plugin", configPath.string());
			}
		}

		if (g_pluginFilenames.empty()) {
			g_pluginFilenames.emplace_back(kDefaultPluginName);
		}
	}

	bool MatchesConfiguredPlugin(const char* a_filename)
	{
		if (!a_filename) {
			return false;
		}

		for (const auto& name : g_pluginFilenames) {
			if (_stricmp(name.c_str(), a_filename) == 0) {
				return true;
			}
		}

		return false;
	}

	TESQuest* ResolveInspectQuest()
	{
		for (const auto& name : g_pluginFilenames) {
			if (auto form = RE::TESDataHandler::GetSingleton()->LookupForm(kInspectQuestFormID, name)) {
				logger::info("Inspect quest resolved from {}", name);
				return static_cast<TESQuest*>(form);
			}
		}

		logger::error("Failed to resolve inspect quest from configured plugin list");
		return nullptr;
	}
}

PlayerCharacter* p;
TESQuest* inspectQuest;
REL::Relocation<uintptr_t> ptr_SetupSpecialIdle{ REL::ID(1379254), 0xDA };
REL::Relocation<uintptr_t> ptr_SetupSpecialIdle2{ REL::ID(760592), 0x3C };
static uintptr_t SetupSpecialIdleOrig;

bool HookedSetupSpecialIdle(AIProcess* ai, Actor& a, DEFAULT_OBJECT obj, TESIdleForm* idle, bool b, TESObjectREFR* target)
{
	if (idle && idle->sourceFiles.array) {
		for (auto fileit = idle->sourceFiles.array->begin(); fileit != idle->sourceFiles.array->end(); ++fileit) {
			if (MatchesConfiguredPlugin((*fileit)->filename)) {
				if (inspectQuest) {
					inspectQuest->currentStage = 1;
					inspectQuest->SetStage(1);
				} else if (!g_warnedMissingQuest) {
					logger::warn("Inspect quest was not resolved; SeamlessInspect cannot skip equip animation");
					g_warnedMissingQuest = true;
				}
				break;
			}
		}
	}
	typedef bool (*FnSetupSpecialIdle)(AIProcess*, Actor&, DEFAULT_OBJECT, TESIdleForm*, bool, TESObjectREFR*);
	FnSetupSpecialIdle fn = (FnSetupSpecialIdle)SetupSpecialIdleOrig;
	return fn ? (*fn)(ai, a, obj, idle, b, target) : false;
}

class AnimationGraphEventWatcher
{
public:
	typedef BSEventNotifyControl (AnimationGraphEventWatcher::*FnProcessEvent)(BSAnimationGraphEvent& evn, BSTEventSource<BSAnimationGraphEvent>* dispatcher);

	BSEventNotifyControl HookedProcessEvent(BSAnimationGraphEvent& evn, BSTEventSource<BSAnimationGraphEvent>* src)
	{
		if (evn.tag == "IdleStop") {
			if (inspectQuest && inspectQuest->currentStage == 1) {
				BGSAnimationSystemUtils::InitializeActorInstant(*p, false);
				p->UpdateAnimation(0.2f);
				inspectQuest->SetStage(0);
			}
		}
		FnProcessEvent fn = fnHash.at(*(uintptr_t*)this);
		return fn ? (this->*fn)(evn, src) : BSEventNotifyControl::kContinue;
	}

	void HookSink()
	{
		uintptr_t vtable = *(uintptr_t*)this;
		auto it = fnHash.find(vtable);
		if (it == fnHash.end()) {
			FnProcessEvent fn = SafeWrite64Function(vtable + 0x8, &AnimationGraphEventWatcher::HookedProcessEvent);
			fnHash.insert(std::pair<uintptr_t, FnProcessEvent>(vtable, fn));
		}
	}

protected:
	static unordered_map<uintptr_t, FnProcessEvent> fnHash;
};
unordered_map<uintptr_t, AnimationGraphEventWatcher::FnProcessEvent> AnimationGraphEventWatcher::fnHash;

void InitializePlugin()
{
	p = PlayerCharacter::GetSingleton();
	if (!p) {
		logger::critical("Failed to acquire PlayerCharacter singleton; SeamlessInspect disabled");
		return;
	}

	inspectQuest = ResolveInspectQuest();
	((AnimationGraphEventWatcher*)((uint64_t)p + 0x38))->HookSink();
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
#ifndef NDEBUG
	auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
	auto path = logger::log_directory();
	if (!path) {
		return false;
	}

	*path /= fmt::format(FMT_STRING("{}.log"), Version::PROJECT);
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

#ifndef NDEBUG
	log->set_level(spdlog::level::trace);
#else
	log->set_level(spdlog::level::info);
	log->flush_on(spdlog::level::info);
#endif

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("%g(%#): [%^%l%$] %v"s);

	logger::info(FMT_STRING("{} v{}"), Version::PROJECT, Version::NAME);

	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = Version::PROJECT.data();
	a_info->version = Version::MAJOR;

	if (a_f4se->IsEditor()) {
		logger::critical("loaded in editor"sv);
		return false;
	}

	const auto ver = a_f4se->RuntimeVersion();
	if (ver < F4SE::RUNTIME_1_10_162) {
		logger::critical(FMT_STRING("unsupported runtime v{}"), ver.string());
		return false;
	}

	F4SE::AllocTrampoline(8 * 8);

	return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);
	LoadPluginFilenames();

	F4SE::Trampoline& trampoline = F4SE::GetTrampoline();
	SetupSpecialIdleOrig = trampoline.write_call<5>(ptr_SetupSpecialIdle.address(), &HookedSetupSpecialIdle);
	trampoline.write_call<5>(ptr_SetupSpecialIdle2.address(), &HookedSetupSpecialIdle);

	const F4SE::MessagingInterface* message = F4SE::GetMessagingInterface();
	message->RegisterListener([](F4SE::MessagingInterface::Message* msg) -> void {
		if (msg->type == F4SE::MessagingInterface::kGameDataReady) {
			InitializePlugin();
		}
	});

	return true;
}
