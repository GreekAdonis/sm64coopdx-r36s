#include <unordered_set>

#include "dynos.cpp.h"

extern "C" {
#include "engine/level_script.h"
#include "game/skybox.h"
}

struct OverrideLevelScript {
    const void *originalScript;
    const void *newScript;
    GfxData *gfxData;
};

static std::vector<OverrideLevelScript> &DynosOverrideLevelScripts() {
    static std::vector<OverrideLevelScript> sDynosOverrideLevelScripts;
    return sDynosOverrideLevelScripts;
}

std::vector<std::pair<std::string, GfxData *>> &DynOS_Lvl_GetArray() {
    static std::vector<std::pair<std::string, GfxData *>> sDynosCustomLevelScripts;
    return sDynosCustomLevelScripts;
}

// Membership filter for DynOS_Lvl_Override().
//
// That function runs per level-script command and walked every override entry
// and then every script of every custom level -- a nested loop -- to discover,
// almost always, that the command it was handed is a plain vanilla one that
// matches nothing. It measured 0.16% of samples in run 21's flood window.
//
// This is deliberately a filter rather than a map, because the first loop cannot
// be expressed as a lookup: it reassigns aCmd on a match and keeps going, so a
// later entry can match the value an earlier one just produced, and the *last*
// match wins rather than the first. Reproducing that with a map would mean
// walking a chain and would be easy to get subtly wrong.
//
// A filter needs none of that. Every comparison in either loop is against a
// pointer in this set, so if aCmd is not in the set neither loop can match on
// its first iteration -- and since only a match mutates aCmd, neither can match
// on any later one either. The loops are then provably no-ops and the function
// is just `return aCmd`. When aCmd *is* in the set, the original code runs
// unchanged.
static std::unordered_set<const void *> &DynosLvlOverrideKeys() {
    static std::unordered_set<const void *> sDynosLvlOverrideKeys;
    return sDynosLvlOverrideKeys;
}

static bool sDynosLvlOverrideKeysDirty = true;

static void DynOS_Lvl_InvalidateOverrideKeys() {
    sDynosLvlOverrideKeysDirty = true;
}

LevelScript* DynOS_Lvl_GetScript(const char* aScriptEntryName) {
    auto& _CustomLevelScripts = DynOS_Lvl_GetArray();
    for (size_t i = 0; i < _CustomLevelScripts.size(); ++i) {
        auto& pair = _CustomLevelScripts[i];
        if (pair.first == aScriptEntryName) {
            auto& newScripts = pair.second->mLevelScripts;
            auto& newScriptNode = newScripts[newScripts.Count() - 1];
            return newScriptNode->mData;
        }
    }
    return NULL;
}

void DynOS_Lvl_ModShutdown() {
    DynOS_Level_Unoverride();

    auto& _CustomLevelScripts = DynOS_Lvl_GetArray();
    if (!_CustomLevelScripts.empty()) {
        for (auto& pair : _CustomLevelScripts) {
            DynOS_Tex_Invalid(pair.second);
            Delete(pair.second);
        }
        _CustomLevelScripts.clear();
    }

    auto& _OverrideLevelScripts = DynosOverrideLevelScripts();
    _OverrideLevelScripts.clear();
    DynOS_Lvl_InvalidateOverrideKeys();
}

void DynOS_Lvl_Activate(s32 modIndex, const SysPath &aFilename, const char *aLevelName) {
    auto& _CustomLevelScripts = DynOS_Lvl_GetArray();
    auto& _OverrideLevelScripts = DynosOverrideLevelScripts();

    // make sure vanilla levels were parsed
    DynOS_Level_Init();

    // check for duplicates
    for (auto &customLevel : _CustomLevelScripts) {
        if (customLevel.first == aLevelName) {
            return;
        }
    }

    std::string levelName = aLevelName;

    GfxData* _Node = DynOS_Lvl_LoadFromBinary(aFilename, levelName.c_str());
    if (!_Node) {
        return;
    }

    // remember index
    _Node->mModIndex = modIndex;

    // Add to levels
    _CustomLevelScripts.emplace_back(levelName, _Node);
    DynOS_Lvl_InvalidateOverrideKeys();
    DynOS_Tex_Valid(_Node);

    // Override vanilla script
    auto& newScripts = _Node->mLevelScripts;
    if (newScripts.Count() <= 0) {
        PrintError("Could not find level scripts: '%s'", aLevelName);
        return;
    }

    auto& newScriptNode = newScripts[newScripts.Count() - 1];
    const void* originalScript = DynOS_Builtin_ScriptPtr_GetFromName(newScriptNode->mName.begin());
    if (originalScript == NULL) {
        return;
    }

    DynOS_Level_Override((void*)originalScript, newScriptNode->mData, modIndex);
    _OverrideLevelScripts.push_back({ originalScript, newScriptNode->mData, _Node});
    DynOS_Lvl_InvalidateOverrideKeys();
}

GfxData* DynOS_Lvl_GetActiveGfx(void) {
    auto& _CustomLevelScripts = DynOS_Lvl_GetArray();
    for (auto &lvlEntry : _CustomLevelScripts) {
        auto& gfxData = lvlEntry.second;
        auto& scripts = gfxData->mLevelScripts;
        for (auto& s : scripts) {
            if (gLevelScriptActive == s->mData) {
                return gfxData;
            }
        }
    }
    return NULL;
}

const char* DynOS_Lvl_GetToken(u32 index) {
    GfxData* gfxData = DynOS_Lvl_GetActiveGfx();
    if (gfxData == NULL) {
        return NULL;
    }

    // have to 1-index due to to pointer read code
    index = index - 1;

    if (index >= gfxData->mLuaTokenList.Count()) {
        return NULL;
    }

    return gfxData->mLuaTokenList[index].begin();
}

Trajectory* DynOS_Lvl_GetTrajectory(const char* aName) {
    auto& _CustomLevelScripts = DynOS_Lvl_GetArray();

    for (auto& script : _CustomLevelScripts) {
        auto trajectoryNode = script.second->mTrajectories.Find(aName);
        if (trajectoryNode) {
            return trajectoryNode->mData;
        }
    }
    return NULL;
}

void DynOS_Lvl_LoadBackground(void *aPtr) {
    auto& _CustomLevelScripts = DynOS_Lvl_GetArray();

    // ensure this texture list exists
    GfxData* foundGfxData = NULL;
    DataNode<TexData*>* foundList = NULL;
    for (auto& script : _CustomLevelScripts) {
        auto &textureLists = script.second->mTextureLists;
        for (auto& textureList : textureLists) {
            if (textureList == aPtr) {
                foundGfxData = script.second;
                foundList = textureList;
                goto double_break;
            }
        }
    }
double_break:

    if (foundList == NULL) {
        PrintError("Could not find custom background");
        return;
    }

    // Load up custom background
    for (s32 i = 0; i < 80; i++) {
        // find texture
        for (auto& tex : foundGfxData->mTextures) {
            if (tex->mData == foundList->mData[i]) {
                gCustomSkyboxPtrList[i] = (Texture*)tex;
                break;
            }
        }
    }
}

void *DynOS_Lvl_Override(void *aCmd) {
    auto& _OverrideLevelScripts = DynosOverrideLevelScripts();

    // See DynosLvlOverrideKeys(): anything not in the set matches nothing below.
    auto& _Keys = DynosLvlOverrideKeys();
    if (sDynosLvlOverrideKeysDirty) {
        _Keys.clear();
        for (auto& overrideStruct : _OverrideLevelScripts) {
            _Keys.insert(overrideStruct.originalScript);
            _Keys.insert(overrideStruct.newScript);
        }
        for (auto& script : DynOS_Lvl_GetArray()) {
            if (!script.second) { continue; }
            for (auto& s : script.second->mLevelScripts) {
                if (s) { _Keys.insert(s->mData); }
            }
        }
        sDynosLvlOverrideKeysDirty = false;
    }
    if (_Keys.find(aCmd) == _Keys.end()) { return aCmd; }

    for (auto& overrideStruct : _OverrideLevelScripts) {
        if (aCmd == overrideStruct.originalScript || aCmd == overrideStruct.newScript) {
            aCmd = (void*)overrideStruct.newScript;
            gLevelScriptModIndex = overrideStruct.gfxData->mModIndex;
            gLevelScriptActive = (LevelScript*)aCmd;
        }
    }

    auto& _CustomLevelScripts = DynOS_Lvl_GetArray();
    for (auto& script : _CustomLevelScripts) {
        auto& scripts = script.second->mLevelScripts;
        for (auto& s : scripts) {
            if (aCmd == s->mData) {
                gLevelScriptModIndex = script.second->mModIndex;
                gLevelScriptActive = (LevelScript*)aCmd;
            }
        }
    }

    return aCmd;
}
