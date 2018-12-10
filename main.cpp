#include "../skse64/PluginAPI.h"
#include "../skse64/GameData.h"
#include "../skse64_common/Utilities.h"
#include "../skse64_common/skse_version.h"

#include "../skse64_common/SafeWrite.h"

#include "BGSSoundCategory.h"
#include "Simpleini.h"

#include <ShlObj.h>

#include <cinttypes>

IDebugLog	gLog;

PluginHandle					g_pluginHandle = kPluginHandle_Invalid;
SKSEMessagingInterface			* g_messaging = nullptr;

struct SoundCategoryInfo
{
	std::string PluginName;
	uint32_t LocalFormId;
	TES::BGSSoundCategory * Category;

	SoundCategoryInfo(std::string name, uint32_t formid, TES::BGSSoundCategory * cat) : PluginName(name), LocalFormId(formid), Category(cat)
	{
	}
};

CSimpleIniA snctIni;
std::map<uint32_t, SoundCategoryInfo> soundCategories;

typedef bool(*_BGSSoundCategory_LoadForm)(TES::BGSSoundCategory * soundCategory, ModInfo * modInfo);
RelocPtr<_BGSSoundCategory_LoadForm> vtbl_BGSSoundCategory_LoadForm(0x01591060); // ::LoadForm = vtable[6] in TESForm derived classes
_BGSSoundCategory_LoadForm orig_BGSSoundCategory_LoadForm;

typedef bool(*_BSISoundCategory_SetVolume)(BSISoundCategory * thisPtr, float volume);
RelocPtr<_BSISoundCategory_SetVolume> vtbl_BSISoundCategory_SetVolume(0x01591260); // ::SetVolume = vtable[3] in ??_7BGSSoundCategory@@6B@_1 (BSISoundCategory)

typedef bool(*_INIPrefSettingCollection_SaveFromMenu)(__int64 thisPtr, __int64 unk1, char * fileName, __int64 unk2);
RelocPtr<_INIPrefSettingCollection_SaveFromMenu> vtbl_INIPrefSettingCollection_SaveFromMenu(0x0154FB28); // ::SaveFromMenu??? = vtable[8]
_INIPrefSettingCollection_SaveFromMenu orig_INIPrefSettingCollection_SaveFromMenu;

bool hk_INIPrefSettingCollection_SaveFromMenu(__int64 thisPtr, __int64 unk1, char * fileName, __int64 unk2)
{
	const bool retVal = orig_INIPrefSettingCollection_SaveFromMenu(thisPtr, unk1, fileName, unk2);

	//_MESSAGE("SaveFromMenu called filename %s", fileName);

	for (auto& soundCategory : soundCategories)
	{
		char localFormIdHex[9];
		sprintf_s(localFormIdHex, sizeof(localFormIdHex), "%08X", soundCategory.second.LocalFormId);
		snctIni.SetDoubleValue(soundCategory.second.PluginName.c_str(), localFormIdHex, static_cast<double>(soundCategory.second.Category->ingameVolume),
			soundCategory.second.Category->GetName(), true);
	}

	const std::string& runtimePath = GetRuntimeDirectory();

	const SI_Error saveRes = snctIni.SaveFile((runtimePath + R"(Data\SKSE\plugins\SNCTSave.ini)").c_str());

	if (saveRes < 0)
	{
		_MESSAGE("warning: unable to save snct ini");
	}

	return retVal;
}

bool hk_BGSSoundCategory_LoadForm(TES::BGSSoundCategory * soundCategory, ModInfo * modInfo)
{
	const bool result = orig_BGSSoundCategory_LoadForm(soundCategory, modInfo);

	if (result)
	{
		//_MESSAGE("[%s] BGSSoundCategory_LoadForm(0x%016" PRIXPTR ", 0x%016" PRIXPTR ") - loaded sound category for formid %08X and name %s", modInfo->name, soundCategory, modInfo, soundCategory->formID, soundCategory->fullName.GetName());
		if (soundCategory->flags & 0x2)
		{
			//_MESSAGE("menu flag set, flagging for save");
			uint32_t localFormId = soundCategory->formID & 0x00FFFFFF;
			// esl
			if ((soundCategory->formID & 0xFF000000) == 0xFE000000)
			{
				localFormId = localFormId & 0x00000FFF;
			}
			SoundCategoryInfo snct(modInfo->name, localFormId, soundCategory);
			soundCategories.insert(std::make_pair(soundCategory->formID, snct));
		}
		else
		{
			//_MESSAGE("menu flag not set, unflagging for save if form was flagged for save in prior plugin");
			soundCategories.erase(soundCategory->formID);
		}
	}
	else
	{
		_MESSAGE("sound category load error????");
	}

	return result;
}

void LoadVolumes()
{
	//_MESSAGE("game has loaded, setting volumes");
	for (auto& soundCategory : soundCategories)
	{
		char localFormIdHex[9];
		sprintf_s(localFormIdHex, sizeof(localFormIdHex), "%08X", soundCategory.second.LocalFormId);
		const auto vol = snctIni.GetDoubleValue(soundCategory.second.PluginName.c_str(), localFormIdHex, -1.0);

		if (vol != -1.0)
		{
			//_MESSAGE("setting volume for formid %08X", soundCategory.second.Category->formID);
			BSISoundCategory * soundCatInterface = &soundCategory.second.Category->soundCategory;

			(*vtbl_BSISoundCategory_SetVolume)(soundCatInterface, static_cast<float>(vol));
		}
	}
}

void SKSEMessageHandler(SKSEMessagingInterface::Message * message)
{
	switch (message->type)
	{
	case SKSEMessagingInterface::kMessage_DataLoaded:
		{
			LoadVolumes();
		}
		break;
	default: 
		break;
	}
}

extern "C" {

	bool SKSEPlugin_Query(const SKSEInterface * skse, PluginInfo * info)
	{
		gLog.OpenRelative(CSIDL_MYDOCUMENTS, R"(\My Games\Skyrim Special Edition\SKSE\SNCTSave.log)");
#ifdef _DEBUG
		gLog.SetLogLevel(IDebugLog::kLevel_DebugMessage);
#else
		gLog.SetLogLevel(IDebugLog::kLevel_Message);
#endif		

		_MESSAGE("SNCTSave");

		// populate info structure
		info->infoVersion = PluginInfo::kInfoVersion;
		info->name = "SNCTSave plugin";
		info->version = 1;

		g_pluginHandle = skse->GetPluginHandle();


		if (skse->isEditor)
		{
			_MESSAGE("loaded in editor, marking as incompatible");
			return false;
		}
		else if (skse->runtimeVersion != RUNTIME_VERSION_1_5_62)
		{
			_FATALERROR("unsupported runtime version %08X", skse->runtimeVersion);
			return false;
		}

		g_messaging = (SKSEMessagingInterface *)skse->QueryInterface(kInterface_Messaging);
		if (!g_messaging) {
			_ERROR("couldn't get messaging interface, disabling plugin");
			return false;
		}

		return true;
	}

	bool SKSEPlugin_Load(const SKSEInterface * skse) 
	{
		if (g_messaging)
			g_messaging->RegisterListener(g_pluginHandle, "SKSE", SKSEMessageHandler);
		
		_MESSAGE("- save added sound categories -");

		const std::string& runtimePath = GetRuntimeDirectory();

		const SI_Error loadRes = snctIni.LoadFile((runtimePath + R"(Data\SKSE\plugins\SNCTSave.ini)").c_str());

		if (loadRes < 0)
		{
			_MESSAGE("unable to load SNCT ini, disabling patch");
			return false;
		}

		_MESSAGE("hooking vtbls");
		orig_INIPrefSettingCollection_SaveFromMenu = *vtbl_INIPrefSettingCollection_SaveFromMenu;
		SafeWrite64(vtbl_INIPrefSettingCollection_SaveFromMenu.GetUIntPtr(), GetFnAddr(hk_INIPrefSettingCollection_SaveFromMenu));
		orig_BGSSoundCategory_LoadForm = *vtbl_BGSSoundCategory_LoadForm;
		SafeWrite64(vtbl_BGSSoundCategory_LoadForm.GetUIntPtr(), GetFnAddr(hk_BGSSoundCategory_LoadForm));
		_MESSAGE("success");
		_MESSAGE("all patches applied");

		return true;
	}
};