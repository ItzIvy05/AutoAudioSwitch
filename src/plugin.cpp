#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <roapi.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <semaphore>

#include <ModConfigUI/Localization.h>
#include <ModConfigUI/ModConfigUI.h>

using REX::W32::ComPtr;

namespace
{
	// XAudio2 2.7 opens a fixed device, this hands it a stream Windows moves between devices itself
	namespace StreamRouting
	{
		REL::Relocation<HRESULT(STDMETHODCALLTYPE*)(IMMDevice*, REFIID, DWORD, PROPVARIANT*, void**)> _Activate;

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

		HRESULT STDMETHODCALLTYPE HookedActivate(IMMDevice* a_device, REFIID a_riid, DWORD a_context, PROPVARIANT* a_params, void** a_interface)
		{
			REX::W32::MEMORY_BASIC_INFORMATION caller{};
			if (REX::W32::VirtualQuery(_ReturnAddress(), &caller, sizeof(caller)) && caller.allocationBase == REX::W32::GetModuleHandleW(L"XAudio2_7.dll") && a_riid == __uuidof(IAudioClient) && a_interface) {
				const auto result = ActivateRouted(a_interface);
				if (SUCCEEDED(result)) {
					return result;
				}
				logger::warn("Failed to open a routed audio stream ({:08X}), using the fixed device"sv, static_cast<std::uint32_t>(result));
			}
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

		void RestoreDefaults()
		{
			AppDevice::SetPreferred({});
			Refresh(true);
		}

		constexpr ModConfigUI::Page PAGES[] = { { "$AutoAudioSwitch_General", DrawGeneralPage } };

		constexpr ModConfigUI::ModInfo MOD_INFO{
			.pluginName = "AutoAudioSwitch",
			.displayName = "$AutoAudioSwitch",
			.version = "1.0.3",
			.author = "Parapets",
			.description = "$AutoAudioSwitch_Description",
			.nexusUrl = "https://www.nexusmods.com/skyrimspecialedition/mods/134404",
			.sourceUrl = "https://github.com/Exit-9B/AutoAudioSwitch",
		};

		void Install()
		{
			ModConfigUI::Install(MOD_INFO, PAGES, RestoreDefaults);
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
