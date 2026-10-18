#pragma once

#include "RE/Skyrim.h"
#include "REL/Relocation.h"
#include "SKSE/SKSE.h"
#include <spdlog/spdlog.h>

using namespace std::literals;

namespace logger = SKSE::log;

// CommonLibSSE-NG names for what ModConfigUI takes from libxse's CommonLib
namespace REL
{
	using Trampoline = SKSE::Trampoline;
}

namespace REX
{
	inline bool UTF16_TO_UTF8(std::wstring_view a_in, std::string& a_out)
	{
		auto out = SKSE::stl::utf16_to_utf8(a_in);
		if (!out) {
			return false;
		}
		a_out = std::move(*out);
		return true;
	}
}
