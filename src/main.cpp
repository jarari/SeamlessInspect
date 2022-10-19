#include "Utilities.h"
using namespace RE;
using std::unordered_map;

PlayerCharacter* p;
TESQuest* inspectQuest;
REL::Relocation<uintptr_t> ptr_SetupSpecialIdle{ REL::ID(1379254), 0xDA };
REL::Relocation<uintptr_t> ptr_SetupSpecialIdle2{ REL::ID(760592), 0x3C };
static uintptr_t SetupSpecialIdleOrig;

bool HookedSetupSpecialIdle(AIProcess* ai, Actor& a, DEFAULT_OBJECT obj, TESIdleForm* idle, bool b, TESObjectREFR* target)
{
	if (idle) {
		for (auto fileit = idle->sourceFiles.array->begin(); fileit != idle->sourceFiles.array->end(); ++fileit) {
			if (strcmp("Inspectweapons.esl", (*fileit)->filename) == 0) {
				inspectQuest->currentStage = 1;
				bool succ = inspectQuest->SetStage(1);
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
		if (evn.animEvent == "IdleStop") {
			if (inspectQuest->currentStage == 1) {
				F4::BGSAnimationSystemUtils::InitializeActorInstant(*p, false);
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
	inspectQuest = (TESQuest*)GetFormFromMod("Inspectweapons.esl", 0x805);
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
	log->flush_on(spdlog::level::warn);
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
