#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <roapi.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <semaphore>

#include <REX/REX/INI.h>

#include <ModConfigUI/Localization.h>
#include <ModConfigUI/ModConfigUI.h>

using REX::W32::ComPtr;

namespace
{
	// XAudio2 2.7 opens a fixed device, this hands it a stream Windows moves between devices itself
	namespace StreamRouting
	{
		REL::Relocation<HRESULT(STDMETHODCALLTYPE*)(IMMDevice*, REFIID, DWORD, PROPVARIANT*, void**)> _Activate;
		REL::Relocation<HRESULT(STDMETHODCALLTYPE*)(IAudioSessionEvents*, AudioSessionDisconnectReason)> _OnSessionDisconnected;
		std::atomic_bool Routed = false;

		class ActivationHandler final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler>
		{
		public:
			HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override
			{
				completed.release();
				return S_OK;
			}

			std::binary_semaphore completed{ 0 };
		};

		HRESULT ActivateRouted(void** a_client)
		{
			const auto handler = Microsoft::WRL::Make<ActivationHandler>();
			ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
			// DEVINTERFACE_AUDIO_RENDER, the default render device
			auto result = ActivateAudioInterfaceAsync(L"{E6327CAD-DCEC-4949-AE8A-991E976A79D2}", __uuidof(IAudioClient), nullptr, handler.Get(), operation.GetAddressOf());
			if (FAILED(result)) {
				return result;
			}

			handler->completed.acquire();
			HRESULT activateResult = E_FAIL;
			ComPtr<IUnknown> client;
			result = operation->GetActivateResult(&activateResult, client.GetAddressOf());
			if (FAILED(result)) {
				return result;
			}
			if (FAILED(activateResult)) {
				return activateResult;
			}
			return client->QueryInterface(__uuidof(IAudioClient), a_client);
		}

		// XAudio2 2.7 shuts its engine down on any session disconnect, Windows moves or reformats a routed stream instead
		HRESULT STDMETHODCALLTYPE OnSessionDisconnected(IAudioSessionEvents* a_events, AudioSessionDisconnectReason a_reason)
		{
			logger::info("Audio session disconnected, reason {}"sv, std::to_underlying(a_reason));
			if (Routed && (a_reason == DisconnectReasonDeviceRemoval || a_reason == DisconnectReasonFormatChanged)) {
				return S_OK;
			}
			return _OnSessionDisconnected(a_events, a_reason);
		}

		void HookSessionEvents(std::uintptr_t a_xaudio)
		{
			// XAudio2_7.dll 9.29.1962 is the only 2.7 build, its IAudioSessionEvents vtable is at 0x22850
			const auto nt = reinterpret_cast<const REX::W32::IMAGE_NT_HEADERS64*>(a_xaudio + reinterpret_cast<const REX::W32::IMAGE_DOS_HEADER*>(a_xaudio)->lfanew);
			REL::Relocation<std::uintptr_t> vtable{ a_xaudio + 0x22850 };
			if (nt->fileHeader.timeDateStamp != 0x4C0643CC || !REL::make_pattern<"48 83 EC 28 48 8B 49 F8 BA 01 00 88 88">().match(*reinterpret_cast<std::uintptr_t*>(vtable.address() + 9 * sizeof(std::uintptr_t)))) {
				logger::error("Unknown XAudio2_7.dll build, device removal is left to XAudio2"sv);
				return;
			}
			_OnSessionDisconnected = vtable.write_vfunc(9, &OnSessionDisconnected);
		}

		struct Module
		{
			std::uintptr_t base;
			std::wstring path;
		};

		Module GetModule(const void* a_address)
		{
			REX::W32::MEMORY_BASIC_INFORMATION memory{};
			wchar_t path[MAX_PATH]{};
			REX::W32::VirtualQuery(a_address, &memory, sizeof(memory));
			REX::W32::GetModuleFileNameW(static_cast<REX::W32::HMODULE>(memory.allocationBase), path, MAX_PATH);
			return { reinterpret_cast<std::uintptr_t>(memory.allocationBase), path };
		}

		// matched by file name, the game loads XAudio2 by name and COM by registry path, so either copy counts
		bool IsXAudio(const std::wstring& a_path)
		{
			return _wcsicmp(a_path.substr(a_path.find_last_of(L'\\') + 1).c_str(), L"XAudio2_7.dll") == 0;
		}

		HRESULT STDMETHODCALLTYPE HookedActivate(IMMDevice* a_device, REFIID a_riid, DWORD a_context, PROPVARIANT* a_params, void** a_interface)
		{
			if (a_riid != __uuidof(IAudioClient) || !a_interface) {
				return _Activate(a_device, a_riid, a_context, a_params, a_interface);
			}

			// overlays such as Steam's game recorder wrap XAudio2's call, so the whole stack is searched for it
			void* frames[16]{};
			const auto count = RtlCaptureStackBackTrace(0, 16, frames, nullptr);
			std::optional<Module> xaudio;
			for (std::uint16_t i = 0; i < count && !xaudio; ++i) {
				auto module = GetModule(frames[i]);
				if (IsXAudio(module.path)) {
					xaudio = std::move(module);
				}
			}

			const auto via = SKSE::stl::utf16_to_utf8(GetModule(_ReturnAddress()).path).value_or(""s);
			if (!xaudio) {
				logger::info("Left an audio stream for {} unrouted"sv, via);
				return _Activate(a_device, a_riid, a_context, a_params, a_interface);
			}

			static std::once_flag hooked;
			std::call_once(hooked, HookSessionEvents, xaudio->base);

			const auto result = ActivateRouted(a_interface);
			Routed = SUCCEEDED(result);
			if (Routed) {
				logger::info("Opened a routed audio stream for XAudio2 via {}"sv, via);
				return result;
			}
			logger::warn("Failed to open a routed audio stream for XAudio2 via {} ({:08X}), using the fixed device"sv, via, static_cast<std::uint32_t>(result));
			return _Activate(a_device, a_riid, a_context, a_params, a_interface);
		}

		void Install()
		{
			// COM stays usable on threads that never initialize it, this one and the menu's
			CO_MTA_USAGE_COOKIE cookie = nullptr;
			CoIncrementMTAUsage(&cookie);

			ComPtr<IMMDeviceEnumerator> enumerator;
			ComPtr<IMMDeviceCollection> devices;
			ComPtr<IMMDevice> device;
			if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.GetAddressOf()))) || FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, devices.GetAddressOf())) || FAILED(devices->Item(0, device.GetAddressOf()))) {
				logger::error("No audio devices found"sv);
				return;
			}

			// every endpoint shares one vtable in MMDevApi.dll, which stays loaded so the patch does too
			REX::W32::LoadLibraryW(L"MMDevApi.dll");
			const auto vtable = *reinterpret_cast<std::uintptr_t*>(device.Get());
			_Activate = REL::Relocation<std::uintptr_t>(vtable).write_vfunc(3, &HookedActivate);
		}
	}

	// movies play through the game's XAudio2 instead of DirectSound, so they follow the routed stream
	namespace Bink
	{
		using BinkOpen2_t = std::int32_t (*(*)(void*, void*))(void*, std::int32_t, std::int32_t, void*, void*);
		using BinkSetSoundSystem2_t = std::int32_t (*)(BinkOpen2_t, void*, void*);

		BinkOpen2_t BinkOpenXAudio27 = nullptr;
		BinkSetSoundSystem2_t BinkSetSoundSystem2 = nullptr;
		REL::Relocation<std::int32_t (*)(void*, std::uintptr_t)> _BinkSetSoundSystem;

		// MoviePlayer calls it as BinkSetSoundSystem(BinkOpenDirectSound, 0)
		std::int32_t SetSoundSystem(void* a_open, std::uintptr_t a_param)
		{
			const auto audioImpl = RE::BSXAudio2GameSound::GetAudioImplementation();
			if (audioImpl && audioImpl->XAudio) {
				return BinkSetSoundSystem2(BinkOpenXAudio27, audioImpl->XAudio, audioImpl->masteringVoice);
			}
			return _BinkSetSoundSystem(a_open, a_param);
		}

		void Install()
		{
			const auto bink2w64 = REX::W32::GetModuleHandleA("bink2w64");
			if (!bink2w64) {
				return;
			}

			BinkOpenXAudio27 = reinterpret_cast<BinkOpen2_t>(REX::W32::GetProcAddress(bink2w64, "BinkOpenXAudio27"));
			BinkSetSoundSystem2 = reinterpret_cast<BinkSetSoundSystem2_t>(REX::W32::GetProcAddress(bink2w64, "BinkSetSoundSystem2"));
			if (!BinkOpenXAudio27 || !BinkSetSoundSystem2) {
				return;
			}

			_BinkSetSoundSystem = SKSE::PatchIAT(&SetSoundSystem, "bink2w64.dll", "BinkSetSoundSystem");
		}
	}

	// the output device Windows routes this process to, the same setting as "App volume and device preferences"
	namespace AppDevice
	{
		using Microsoft::WRL::Wrappers::HString;
		using Microsoft::WRL::Wrappers::HStringReference;

		struct Device
		{
			std::wstring id;
			std::string name;
		};

		constexpr std::wstring_view MMDEVAPI_PREFIX = L"\\\\?\\SWD#MMDEVAPI#";
		constexpr std::wstring_view RENDER_SUFFIX = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";

		// Windows.Media.Internal.AudioPolicyConfig is undocumented
		constexpr IID POLICY_CONFIG{ 0xAB3D4648, 0xE242, 0x459F, { 0xB0, 0x2F, 0x54, 0x1C, 0x70, 0x30, 0x63, 0x24 } };

		// IInspectable, then 19 methods before these two
		using SetPersistedDefaultAudioEndpoint = HRESULT(IInspectable*, UINT, EDataFlow, ERole, HSTRING);
		using GetPersistedDefaultAudioEndpoint = HRESULT(IInspectable*, UINT, EDataFlow, ERole, HSTRING*);
		constexpr std::ptrdiff_t SET_PERSISTED_DEFAULT = 25;
		constexpr std::ptrdiff_t GET_PERSISTED_DEFAULT = 26;

		IInspectable* GetPolicyConfig()
		{
			static IInspectable* const policyConfig = []()
			{
				IInspectable* result = nullptr;
				if (FAILED(RoGetActivationFactory(HStringReference(L"Windows.Media.Internal.AudioPolicyConfig").Get(), POLICY_CONFIG, reinterpret_cast<void**>(&result)))) {
					logger::error("Per-app audio device settings are not available"sv);
				}
				return result;
			}();
			return policyConfig;
		}

		ComPtr<IMMDeviceEnumerator> GetEnumerator()
		{
			ComPtr<IMMDeviceEnumerator> enumerator;
			CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.GetAddressOf()));
			return enumerator;
		}

		std::wstring GetID(IMMDevice* a_device)
		{
			LPWSTR id = nullptr;
			if (FAILED(a_device->GetId(&id))) {
				return {};
			}
			std::wstring result{ id };
			REX::W32::CoTaskMemFree(id);
			return result;
		}

		std::vector<Device> GetDevices()
		{
			std::vector<Device> devices;
			const auto enumerator = GetEnumerator();
			ComPtr<IMMDeviceCollection> collection;
			if (!enumerator.Get() || FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.GetAddressOf()))) {
				return devices;
			}

			UINT count = 0;
			collection->GetCount(&count);
			for (UINT i = 0; i < count; ++i) {
				ComPtr<IMMDevice> device;
				ComPtr<IPropertyStore> properties;
				if (FAILED(collection->Item(i, device.GetAddressOf())) || FAILED(device->OpenPropertyStore(STGM_READ, properties.GetAddressOf()))) {
					continue;
				}

				PROPVARIANT name;
				PropVariantInit(&name);
				if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) {
					devices.push_back({ GetID(device.Get()), SKSE::stl::utf16_to_utf8(name.pwszVal).value_or(""s) });
				}
				PropVariantClear(&name);
			}
			return devices;
		}

		std::wstring GetDefault()
		{
			const auto enumerator = GetEnumerator();
			ComPtr<IMMDevice> device;
			if (!enumerator.Get() || FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf()))) {
				return {};
			}
			return GetID(device.Get());
		}

		// empty means the Windows default
		std::wstring GetPreferred()
		{
			const auto policyConfig = GetPolicyConfig();
			HString value;
			if (!policyConfig || FAILED(REL::RelocateVirtual<GetPersistedDefaultAudioEndpoint>(0, 0, GET_PERSISTED_DEFAULT, GET_PERSISTED_DEFAULT, policyConfig, GetCurrentProcessId(), eRender, eMultimedia, value.GetAddressOf()))) {
				return {};
			}

			std::wstring_view id{ value.GetRawBuffer(nullptr) };
			if (id.starts_with(MMDEVAPI_PREFIX)) {
				id.remove_prefix(MMDEVAPI_PREFIX.size());
			}
			if (id.ends_with(RENDER_SUFFIX)) {
				id.remove_suffix(RENDER_SUFFIX.size());
			}
			return std::wstring{ id };
		}

		void SetPreferred(const std::wstring& a_id)
		{
			const auto policyConfig = GetPolicyConfig();
			if (!policyConfig) {
				return;
			}

			// an empty path is a null HSTRING, which clears the setting
			const auto path = a_id.empty() ? std::wstring{} : std::format(L"{}{}{}", MMDEVAPI_PREFIX, a_id, RENDER_SUFFIX);
			const HStringReference value(path.c_str(), static_cast<unsigned int>(path.size()));
			for (const auto role : { eConsole, eMultimedia }) {
				if (FAILED(REL::RelocateVirtual<SetPersistedDefaultAudioEndpoint>(0, 0, SET_PERSISTED_DEFAULT, SET_PERSISTED_DEFAULT, policyConfig, GetCurrentProcessId(), eRender, role, value.Get()))) {
					logger::error("Failed to set the audio device"sv);
				}
			}
		}
	}

	namespace Menu
	{
		using ModConfigUI::Localization::Get;

		// 0 SKSE Menu Framework, 1 F.U.C.K, 2 both
		REX::INI::I32<> MenuFramework{ "General", "iMenuFramework", 2 };

		struct State
		{
			std::vector<AppDevice::Device> devices;
			std::wstring preferred;
			std::wstring fallback;
			std::chrono::steady_clock::time_point refreshed;
		};

		State Cache;

		// the device list is read once a second, not every frame
		void Refresh(bool a_force)
		{
			const auto now = std::chrono::steady_clock::now();
			if (!a_force && now - Cache.refreshed < std::chrono::seconds(1)) {
				return;
			}
			Cache.devices = AppDevice::GetDevices();
			Cache.preferred = AppDevice::GetPreferred();
			Cache.fallback = AppDevice::GetDefault();
			Cache.refreshed = now;
		}

		void DrawGeneralPage(ModConfigUI::Renderer& a_renderer)
		{
			Refresh(false);

			std::vector<const char*> items{ Get("$AutoAudioSwitch_Auto") };
			std::int32_t current = 0;
			for (const auto& device : Cache.devices) {
				items.push_back(device.name.c_str());
				if (device.id == Cache.preferred) {
					current = static_cast<std::int32_t>(items.size()) - 1;
				}
			}

			a_renderer.SeparatorText(Get("$AutoAudioSwitch_Output"));
			if (a_renderer.Combo(Get("$AutoAudioSwitch_OutputDevice"), &current, items.data(), static_cast<std::int32_t>(items.size()), 0, Get("$AutoAudioSwitch_OutputDevice_Tooltip"))) {
				AppDevice::SetPreferred(current == 0 ? std::wstring{} : Cache.devices[current - 1].id);
				Refresh(true);
			}

			// looked up after the combo, a change above replaces the device list
			auto playing = std::ranges::find(Cache.devices, Cache.preferred, &AppDevice::Device::id);
			if (playing == Cache.devices.end()) {
				playing = std::ranges::find(Cache.devices, Cache.fallback, &AppDevice::Device::id);
			}
			a_renderer.LabeledText(Get("$AutoAudioSwitch_PlayingOn"), playing != Cache.devices.end() ? playing->name.c_str() : Get("$AutoAudioSwitch_NoDevice"));
		}

		constexpr ModConfigUI::Page PAGES[] = { { "$AutoAudioSwitch_General", DrawGeneralPage } };

		constexpr ModConfigUI::ModInfo MOD_INFO{
			.pluginName = "AutoAudioSwitch",
			.displayName = "$AutoAudioSwitch",
			.version = "1.0.3",
			.author = "Parapets",
			.description = "$AutoAudioSwitch_Description",
			.sourceUrl = "https://github.com/ItzIvy05/AutoAudioSwitch",
		};

		void Install()
		{
			const auto ini = REX::INI::SettingStore::GetSingleton();
			ini->Init("Data/SKSE/Plugins/AutoAudioSwitch.ini", "");
			ini->Load();

			const auto framework = MenuFramework.GetValue();
			ModConfigUI::Install(MOD_INFO, PAGES, nullptr, framework != 1, framework != 0);
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	if (!REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_6_1130)) {
		return false;
	}

	SKSE::Init(a_skse);

	StreamRouting::Install();
	Bink::Install();

	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == SKSE::MessagingInterface::kPostLoad) {
			Menu::Install();
		}
	});

	return true;
}
