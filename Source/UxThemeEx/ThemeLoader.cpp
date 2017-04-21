﻿#include "ThemeLoader.h"

#include "DpiInfo.h"
#include "BorderFill.h"
#include "Debug.h"
#include "Handle.h"
#include "RenderObj.h"
#include "TextDraw.h"
#include "Utils.h"
#include "UxThemeFile.h"
#include "UxThemeHelpers.h"

#include <algorithm>
#include <array>
#include <intsafe.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <vssym32.h>

namespace uxtheme
{

struct VISUALSTYLELOAD
{
    unsigned cbStruct;
    unsigned ulFlags;
    HMODULE hInstVS;
    wchar_t const* pszColorVariant;
    wchar_t const* pszSizeVariant;
    IParserCallBack* pfnCB;
};

static HRESULT VSLoad(VISUALSTYLELOAD* pvsl, BOOL fIsLiteVisualStyle, BOOL fHighContrast)
{
    if (!pvsl
        || pvsl->cbStruct != sizeof(VISUALSTYLELOAD)
        || !pvsl->hInstVS
        || !pvsl->pfnCB
        || !pvsl->pszColorVariant
        || !*pvsl->pszColorVariant
        || !pvsl->pszSizeVariant
        || !*pvsl->pszSizeVariant)
        return E_INVALIDARG;

    auto unpack = make_unique_nothrow<CVSUnpack>();
    if (!unpack)
        return E_OUTOFMEMORY;

    ENSURE_HR(unpack->Initialize(pvsl->hInstVS, 0, pvsl->ulFlags & 1,
                                 fIsLiteVisualStyle));
    ENSURE_HR(unpack->LoadRootMap(pvsl->pfnCB));
    ENSURE_HR(unpack->LoadClassDataMap(pvsl->pszColorVariant,
                                       pvsl->pszSizeVariant, pvsl->pfnCB));
    ENSURE_HR(unpack->LoadBaseClassDataMap(pvsl->pfnCB));
    ENSURE_HR(unpack->LoadAnimationDataMap(pvsl->pfnCB));
    return S_OK;
}

static unsigned const DefaultColors[31] = {
    0xC8D0D4,
    0xA56E3A,
    0x6A240A,
    0x808080,
    0xC8D0D4,
    0xFFFFFF,
    0x000000,
    0x000000,
    0x000000,
    0xFFFFFF,
    0xC8D0D4,
    0xC8D0D4,
    0x808080,
    0x6A240A,
    0xFFFFFF,
    0xC8D0D4,
    0x808080,
    0x808080,
    0x000000,
    0xC8D0D4,
    0xFFFFFF,
    0x404040,
    0xC8D0D4,
    0x000000,
    0xE1FFFF,
    0xB5B5B5,
    0x800000,
    0xF0CAA6,
    0xC0C0C0,
    0xE1D3CE,
    0xF0F4F4,
};

static void SetFont(LOGFONTW *plf, wchar_t const* lszFontName, int iPointSize)
{
    fill_zero(*plf);
    plf->lfWeight = 400;
    plf->lfCharSet = 1;
    plf->lfHeight = -MulDiv(iPointSize, 96, 72);
    StringCchCopyW(plf->lfFaceName, countof(plf->lfFaceName), lszFontName);
}

static HRESULT InitThemeMetrics(LOADTHEMEMETRICS* tm)
{
    memset(tm, 0, sizeof(LOADTHEMEMETRICS));
    SetFont(&tm->lfFonts[TMT_CAPTIONFONT - TMT_FIRSTFONT], L"tahoma bold", 8);
    SetFont(&tm->lfFonts[TMT_SMALLCAPTIONFONT - TMT_FIRSTFONT], L"tahoma", 8);
    SetFont(&tm->lfFonts[TMT_MENUFONT - TMT_FIRSTFONT], L"tahoma", 8);
    SetFont(&tm->lfFonts[TMT_STATUSFONT - TMT_FIRSTFONT], L"tahoma", 8);
    SetFont(&tm->lfFonts[TMT_MSGBOXFONT - TMT_FIRSTFONT], L"tahoma", 8);
    SetFont(&tm->lfFonts[TMT_ICONTITLEFONT - TMT_FIRSTFONT], L"tahoma", 8);
    tm->iSizes[TMT_SIZINGBORDERWIDTH - TMT_FIRSTSIZE] = 1;
    tm->iSizes[TMT_SCROLLBARWIDTH - TMT_FIRSTSIZE] = 16;
    tm->iSizes[TMT_SCROLLBARHEIGHT - TMT_FIRSTSIZE] = 16;
    tm->iSizes[TMT_CAPTIONBARWIDTH - TMT_FIRSTSIZE] = 18;
    tm->iSizes[TMT_CAPTIONBARHEIGHT - TMT_FIRSTSIZE] = 19;
    tm->iSizes[TMT_SMCAPTIONBARWIDTH - TMT_FIRSTSIZE] = 12;
    tm->iSizes[TMT_SMCAPTIONBARHEIGHT - TMT_FIRSTSIZE] = 19;
    tm->iSizes[TMT_MENUBARWIDTH - TMT_FIRSTSIZE] = 18;
    tm->iSizes[TMT_MENUBARHEIGHT - TMT_FIRSTSIZE] = 19;
    tm->iSizes[TMT_PADDEDBORDERWIDTH - TMT_FIRSTSIZE] = 0;
    tm->iSizes[10] = 0;
    tm->iStringOffsets[TMT_CSSNAME - TMT_FIRSTSTRING] = 0;
    tm->iStringOffsets[TMT_XMLNAME - TMT_FIRSTSTRING] = 0;
    tm->wsStrings[TMT_CSSNAME - TMT_FIRSTSTRING] = L"";
    tm->wsStrings[TMT_XMLNAME - TMT_FIRSTSTRING] = L"";
    tm->iInts[TMT_MINCOLORDEPTH - TMT_FIRSTINT] = 16;
    memcpy(tm->crColors, DefaultColors, sizeof(tm->crColors));
    return S_OK;
}

CThemeLoader::CThemeLoader()
    : _iGlobalsOffset(0)
    , _iSysMetricsOffset(0)
    , _iGlobalsTextObj(0)
    , _iGlobalsDrawObj(0)
    , _pbLocalData(nullptr)
    , _iLocalLen(0)
    , _iEntryHdrLevel(-1)
    , _fGlobalTheme(0)
    , _hdr(nullptr)
{
    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);

    InitThemeMetrics(&_LoadThemeMetrics);
    _dwPageSize = systemInfo.dwPageSize;
    _wCurrentLangID = 0;
    _iCurrentScreenPpi = 96;
}

HRESULT CThemeLoader::AllocateThemeFileBytes(BYTE* upb, unsigned dwAdditionalLen)
{
    bool overflowsPage = (uintptr_t)upb / _dwPageSize != ((uintptr_t)upb + dwAdditionalLen) / _dwPageSize;

    if (overflowsPage) {
        bool z = dwAdditionalLen <= (unsigned __int64)((BYTE*)_LoadingThemeFile.ThemeHeader() - upb + 0x7FFFFFFF);
        if (!z)
            return E_OUTOFMEMORY;

        SIZE_T s = (SIZE_T)(upb + dwAdditionalLen - (uint64_t)_LoadingThemeFile.ThemeHeader() + 1);
        if (!VirtualAlloc(_LoadingThemeFile.ThemeHeader(), s, MEM_COMMIT, PAGE_READWRITE))
            return E_OUTOFMEMORY;
    }

    return S_OK;
}

HRESULT CThemeLoader::EmitEntryHdr(MIXEDPTRS* u, short propnum, BYTE privnum)
{
    if (_iEntryHdrLevel == 5)
        return E_FAIL;

    if (_LoadingThemeFile.ThemeHeader())
        ENSURE_HR(AllocateThemeFileBytes(u->pb, sizeof(ENTRYHDR)));

    auto hdr = reinterpret_cast<ENTRYHDR*>(u->pb);
    hdr->usTypeNum = propnum;
    hdr->ePrimVal = privnum;
    hdr->dwDataLen = 0;
    RegisterPtr(hdr);

    ++_iEntryHdrLevel;
    u->pb = reinterpret_cast<BYTE*>(hdr + 1);

    _pbEntryHdrs[_iEntryHdrLevel] = hdr;
    return S_OK;
}

int CThemeLoader::EndEntry(MIXEDPTRS* u)
{
    auto hdr = _pbEntryHdrs[_iEntryHdrLevel];

    unsigned len = (uintptr_t)u->pi - (uintptr_t)hdr - 8;
    unsigned alignedLen = Align8(len);

    unsigned padding = alignedLen - len;
    if (_LoadingThemeFile.ThemeHeader() && FAILED(AllocateThemeFileBytes(u->pb, padding)))
        return -1;

    u->pb += padding;
    hdr->dwDataLen = alignedLen;
    --_iEntryHdrLevel;

    return padding;
}

HRESULT CThemeLoader::EmitAndCopyBlock(MIXEDPTRS* u, void const* pSrc, unsigned dwLen)
{
    unsigned paddedLen = Align8(dwLen);
    ENSURE_HR(AllocateThemeFileBytes(u->pb, paddedLen));

    memcpy(u->pb, pSrc, dwLen);
    u->pb += paddedLen;
    return S_OK;
}

BOOL CThemeLoader::IndexExists(
    wchar_t const* pszAppName, wchar_t const* pszClassName, int iPartId, int iStateId)
{
    APPCLASSLOCAL const* matchedClass = nullptr;

    for (auto const& cls : _LocalIndexes) {
        auto currApp = cls.csAppName.c_str();
        auto currCls = cls.csClassName.c_str();

        bool reqName = pszAppName && *pszAppName;
        bool hasName = currApp && *currApp;

        if (((!reqName && !hasName) || (reqName && hasName && AsciiStrCmpI(pszAppName, currApp) == 0)) &&
            AsciiStrCmpI(pszClassName, currCls) == 0) {
            matchedClass = &cls;
            break;
        }
    }

    if (matchedClass) {
        for (auto const& psi : matchedClass->PartStateIndexes) {
            if (psi.iPartId == iPartId && psi.iStateId == iStateId)
                return TRUE;
        }
    }

    return FALSE;
}

HRESULT CThemeLoader::AddMissingParent(
    wchar_t const* pszAppName, wchar_t const* pszClassName, int iPartId, int iStateId)
{
    int beginIdx = GetNextDataIndex();
    unsigned value = 0;
    ENSURE_HR(AddData(12, 12, &value, sizeof(value)));

    int length = GetNextDataIndex() - beginIdx;
    ENSURE_HR(AddIndexInternal(pszAppName, pszClassName, iPartId, iStateId, beginIdx, length));
    return S_OK;
}

HRESULT CThemeLoader::AddIndex(
    wchar_t const* pszAppName, wchar_t const* pszClassName, int iPartId, int iStateId, int iIndex, int iLen)
{
    if (iPartId && !IndexExists(pszAppName, pszClassName, 0, 0))
        ENSURE_HR(AddMissingParent(pszAppName, pszClassName, 0, 0));
    if (iStateId && !IndexExists(pszAppName, pszClassName, iPartId, 0))
        ENSURE_HR(AddMissingParent(pszAppName, pszClassName, iPartId, 0));

    ENSURE_HR(AddIndexInternal(pszAppName, pszClassName, iPartId, iStateId, iIndex, iLen));
    return S_OK;
}

HRESULT CThemeLoader::AddIndexInternal(
    wchar_t const* pszAppName, wchar_t const* pszClassName, int iPartId,
    int iStateId, int iIndex, int iLen)
{
    APPCLASSLOCAL* cls = nullptr;

    for (auto& c : _LocalIndexes) {
        auto currApp = c.csAppName.c_str();
        auto currCls = c.csClassName.c_str();

        bool reqName = pszAppName && *pszAppName;
        bool hasName = currApp && *currApp;

        if (((!reqName && !hasName) || (reqName && hasName && AsciiStrCmpI(pszAppName, currApp) == 0)) &&
            AsciiStrCmpI(pszClassName, currCls) == 0) {
            cls = &c;
            break;
        }
    }

    if (cls) {
        for (auto const& psi : cls->PartStateIndexes) {
            if (psi.iPartId == iPartId && psi.iStateId == iStateId)
                return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        }
    }

    if (!cls) {
        APPCLASSLOCAL c;
        c.csAppName = pszAppName;
        c.csClassName = pszClassName;
        _LocalIndexes.push_back(c);
        cls = &_LocalIndexes.back();
    }

    if (iPartId > cls->iMaxPartNum)
        cls->iMaxPartNum = iPartId;

    PART_STATE_INDEX psi;
    psi.iPartId = iPartId;
    psi.iStateId = iStateId;
    psi.iIndex = iIndex;
    psi.iLen = iLen;
    cls->PartStateIndexes.push_back(psi);

    return S_OK;
}

HRESULT CThemeLoader::AddData(short sTypeNum, unsigned char ePrimVal,
                              void const* pData, unsigned dwLen)
{
    if (ePrimVal == TMT_FONT) {
        unsigned short idx = 0;
        ENSURE_HR(GetFontTableIndex(static_cast<LOGFONTW const*>(pData), &idx));
        pData = &idx;
        dwLen = sizeof(idx);
    }

    return AddDataInternal(sTypeNum, ePrimVal, pData, dwLen);
}

HRESULT CThemeLoader::AddBaseClass(int idClass, int idBaseClass)
{
    _rgBaseClassIds.push_back(idBaseClass);
    return S_OK;
}

int CThemeLoader::AddToDIBDataArray(void* pDIBBits, short width, short height)
{
    DIBDATA d;
    d.pDIBBits = pDIBBits;
    d.dibReuseData.iDIBBitsOffset = 0;
    d.dibReuseData.iWidth = width;
    d.dibReuseData.iHeight = height;
    _rgDIBDataArray.push_back(d);
    return _rgDIBDataArray.size() - 1;
}

HRESULT CThemeLoader::AddDataInternal(short sTypeNum, unsigned char ePrimVal, void const* pData, unsigned dwLen)
{
    if (dwLen + 16 < dwLen || dwLen + 16 > 0x7FFFFFFF - _iLocalLen)
        return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);

    MIXEDPTRS u;
    u.pb = &_pbLocalData[_iLocalLen];

    bool overflowsPage = (uintptr_t)u.pb / _dwPageSize != ((uintptr_t)u.pb + dwLen + 15) / _dwPageSize;

    if ((overflowsPage || _iLocalLen == 0)
        && !VirtualAlloc(_pbLocalData, _iLocalLen + dwLen + 16, MEM_COMMIT, PAGE_READWRITE))
        return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);

    ENSURE_HR(EmitEntryHdr(&u, sTypeNum, ePrimVal));

    if (dwLen) {
        memcpy(u.pb, pData, dwLen);
        u.pb += dwLen;
    }

    _iLocalLen += 8 + dwLen + EndEntry(&u);

    return S_OK;
}

void CThemeLoader::FreeLocalTheme()
{
    if (_pbLocalData) {
        VirtualFree(_pbLocalData, 0, MEM_RELEASE);
        _pbLocalData = nullptr;
        _iLocalLen = 0;
    }

    _LocalIndexes.clear();
    _rgBaseClassIds.clear();
}

HRESULT CThemeLoader::LoadTheme(HMODULE hInst, wchar_t const* pszThemeName,
                                wchar_t const* pszColorParam,
                                wchar_t const*pszSizeParam,
                                HANDLE* phReuseSection, BOOL fGlobalTheme)
{
    if (phReuseSection)
        *phReuseSection = nullptr;

    FreeLocalTheme();
    _fGlobalTheme = fGlobalTheme;
    g_DpiInfo.Clear();
    g_DpiInfo.Ensure(0);

    size_t const MinReserve = 10 * 1024 * 1024;

    HANDLE hFile = CreateFileW(
        pszThemeName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (!hFile)
        return MakeErrorLast();

    size_t fileSize = GetFileSize(hFile, nullptr);
    CloseHandle(hFile);

    _pbLocalData = static_cast<BYTE*>(VirtualAlloc(
        nullptr, std::max(fileSize, MinReserve), MEM_RESERVE, PAGE_READWRITE));

    if (!_pbLocalData)
        return HRESULT_FROM_WIN32(ERROR_OUTOFMEMORY);

    BOOL isLiteStyle = FALSE;
    if (pszThemeName)
        isLiteStyle = StrRStrIW(pszThemeName, nullptr, L"aerolite.msstyles") ? TRUE : FALSE;

    BOOL isHighContrast = FALSE;

    VISUALSTYLELOAD vsl = {};
    vsl.cbStruct = sizeof(vsl);
    vsl.hInstVS = hInst;
    vsl.ulFlags = fGlobalTheme != 0;
    vsl.pszColorVariant = pszColorParam;
    vsl.pszSizeVariant = pszSizeParam;
    vsl.pfnCB = this;
    ENSURE_HR(VSLoad(&vsl, isLiteStyle, isHighContrast));

    ENSURE_HR(PackAndLoadTheme(
        hFile,
        pszThemeName,
        pszColorParam,
        pszSizeParam,
        std::max(fileSize, MinReserve),
        nullptr,
        0,
        nullptr,
        0,
        phReuseSection,
        nullptr));

    return S_OK;
}

HRESULT CThemeLoader::EmitString(
    MIXEDPTRS* u, wchar_t const* pszSrc, unsigned cchSrc, int* piOffSet)
{
    unsigned paddedLen = Align8(2 * (cchSrc + 1));
    ENSURE_HR(AllocateThemeFileBytes(u->pb, paddedLen));
    ENSURE_HR(StringCchCopyW((wchar_t*)u->pb, cchSrc + 1, pszSrc));

    if (piOffSet)
        *piOffSet = (BYTE*)(u->pi) - (BYTE*)(_LoadingThemeFile.ThemeHeader());
    u->pb += paddedLen;

    return S_OK;
}

HRESULT CThemeLoader::EmitObject(
    MIXEDPTRS* u, short propnum, unsigned char privnum, void* pHdr,
    unsigned dwHdrLen, void* pObj, unsigned dwObjLen, CRenderObj* pRender)
{
    ENSURE_HR(EmitEntryHdr(u, propnum, privnum));
    ENSURE_HR(AllocateThemeFileBytes(u->pb, dwHdrLen));

    memcpy(u->pb, pHdr, dwHdrLen);
    RegisterPtr((PARTOBJHDR*)u->pb);
    u->pb += dwHdrLen;

    auto paddedLen = Align8(dwObjLen);
    ENSURE_HR(AllocateThemeFileBytes(u->pb, paddedLen));
    memcpy(u->pb, pObj, dwObjLen);
    if (dwObjLen == sizeof(CTextDraw))
        RegisterPtr((CTextDraw*)u->pb);
    else
        RegisterPtr((CDrawBase*)u->pb);
    u->pb += paddedLen;

    EndEntry(u);

    return S_OK;
}

HRESULT CThemeLoader::GetFontTableIndex(LOGFONTW const* pFont, unsigned short* pIndex)
{
    if (_fontTable.empty()) {
        LOGFONTW t = {};
        _fontTable.push_back(t);
    }

    for (size_t i = 0; i < _fontTable.size(); ++i) {
        if (!memcmp(&_fontTable[i], pFont, sizeof(LOGFONTW))) {
            *pIndex = narrow_cast<unsigned short>(i);
            return S_OK;
        }
    }

    _fontTable.push_back(*pFont);
    *pIndex = narrow_cast<unsigned short>(_fontTable.size() - 1);

    return S_OK;
}

HRESULT CThemeLoader::PackDrawObject(
    MIXEDPTRS* u, CRenderObj* pRender, int iPartId, int iStateId)
{
    int bgType;
    if (pRender->ExternalGetEnumValue(iPartId, iStateId/*0*/, TMT_BGTYPE, &bgType) < 0)
        bgType = BT_BORDERFILL;

    PARTOBJHDR partHdr;
    partHdr.iPartId = iPartId;
    partHdr.iStateId = iStateId/*0*/;

    if (bgType == BT_NONE || bgType == BT_BORDERFILL) {
        CBorderFill borderFill;
        ENSURE_HR(borderFill.PackProperties(
            pRender, bgType == BT_NONE, iPartId, iStateId/*0*/));
        return EmitObject(u, TMT_DRAWOBJ, TMT_DRAWOBJ, &partHdr,
                          sizeof(partHdr), &borderFill, sizeof(borderFill), pRender);
    }

    CMaxImageFile imgFile;
    ENSURE_HR(imgFile.PackProperties(pRender, iPartId, iStateId/*0*/));

    DIBINFO* pScaledDibInfo = nullptr;
    HRESULT hr = imgFile.CreateScaledBackgroundImage(
        pRender, iPartId, iStateId/*0*/, &pScaledDibInfo);
    if (hr < 0)
        return hr;
    if (hr == 0)
        ENSURE_HR(MakeStockObject(pRender, pScaledDibInfo));

    for (int i = 0; ; ++i) {
        auto pdi = imgFile.EnumImageFiles(i);
        if (!pdi)
            break;
        ENSURE_HR(PackImageFileInfo(pdi, &imgFile, u, pRender, iPartId,
                                    iStateId));
    }

    return EmitObject(u, TMT_DRAWOBJ, TMT_DRAWOBJ, &partHdr, sizeof(partHdr), &imgFile,
                      sizeof(CImageFile) + imgFile._iMultiImageCount * sizeof(DIBINFO), pRender);
}

BOOL CThemeLoader::KeyTextPropertyFound(int iStateDataOffset)
{
    for (auto hdr = (ENTRYHDR*)((BYTE*)_LoadingThemeFile.ThemeHeader() + iStateDataOffset);
         hdr->usTypeNum != TMT_JUMPTOPARENT;
         hdr = hdr->Next())
    {
        if (CTextDraw::KeyProperty(hdr->usTypeNum))
            return TRUE;
    }

    return FALSE;
}

HRESULT CThemeLoader::PackTextObjects(
    MIXEDPTRS* uOut, CRenderObj* pRender, int iMaxPart, bool fGlobals)
{
    for (int partId = 0; partId <= iMaxPart; ++partId) {
        int offset = GetPartOffset(pRender, partId);
        if (offset == -1)
            continue;

        auto entryHdr = (ENTRYHDR*)((BYTE*)_LoadingThemeFile.ThemeHeader() + offset);
        if (entryHdr->usTypeNum == TMT_STATEJUMPTABLE) {
            auto jumpTableHdr = (STATEJUMPTABLEHDR*)(entryHdr + 1);
            auto jumpTable = (int*)(jumpTableHdr + 1);

            for (int stateId = 0; stateId <= jumpTableHdr->cStates - 1; ++stateId) {
                int so = jumpTable[stateId];
                if (so != -1 && (fGlobals || KeyTextPropertyFound(so))) {
                    ENSURE_HR(PackTextObject(uOut, pRender, partId, stateId));
                    fGlobals = 0;
                }
            }
        } else if (fGlobals || KeyTextPropertyFound(offset)) {
            ENSURE_HR(PackTextObject(uOut, pRender, partId, 0));
        }
    }

    return S_OK;
}

HRESULT CThemeLoader::PackTextObject(
    MIXEDPTRS* u, CRenderObj* pRender, int iPartId, int iStateId)
{
    PARTOBJHDR partHdr;
    partHdr.iPartId = iPartId;
    partHdr.iStateId = iStateId;

    CTextDraw textDraw;
    ENSURE_HR(textDraw.PackProperties(pRender, iPartId, iStateId));
    return EmitObject(u, TMT_TEXTOBJ, TMT_TEXTOBJ, &partHdr, sizeof(partHdr),
                      &textDraw, sizeof(textDraw), pRender);
}

int CThemeLoader::GetScreenPpi()
{
    return _iCurrentScreenPpi;
}

HRESULT CThemeLoader::PackMetrics()
{
    APPCLASSLOCAL* pac = nullptr;
    for (int i = 0; i < _LocalIndexes.size(); ++i) {
        if (AsciiStrCmpI(_LocalIndexes[i].csClassName.c_str(), L"SysMetrics") == 0) {
            pac = &_LocalIndexes[i];
            break;
        }
    }

    if (pac && pac->PartStateIndexes.size() != 0) {
        PART_STATE_INDEX* index = pac->PartStateIndexes.data();
        auto const* entryHdr = (ENTRYHDR*)&_pbLocalData[index->iIndex];
        auto const* const end = (ENTRYHDR*)&_pbLocalData[index->iIndex + index->iLen];

        while (entryHdr < end && entryHdr->usTypeNum != TMT_JUMPTOPARENT) {
            BYTE const* data = entryHdr->Data();

            switch (entryHdr->ePrimVal) {
            case TMT_STRING:
                _LoadThemeMetrics.wsStrings[entryHdr->usTypeNum - TMT_CSSNAME] = (wchar_t const*)data;
                break;
            case TMT_INT:
                _LoadThemeMetrics.iInts[entryHdr->usTypeNum - TMT_FIRSTINT] = *(DWORD*)data;
                break;
            case TMT_BOOL:
                _LoadThemeMetrics.fBools[entryHdr->usTypeNum - TMT_FIRSTBOOL] = *(BYTE*)data;
                break;
            case TMT_COLOR:
                _LoadThemeMetrics.crColors[entryHdr->usTypeNum - TMT_FIRSTCOLOR] = *(DWORD*)data;
                break;
            case TMT_SIZE:
                _LoadThemeMetrics.iSizes[entryHdr->usTypeNum - TMT_FIRSTSIZE] = *(DWORD*)data;
                break;
            case TMT_FONT:
                _LoadThemeMetrics.lfFonts[entryHdr->usTypeNum - TMT_FIRSTFONT] = _fontTable[*(short*)data];
                break;
            }

            entryHdr = entryHdr->Next();
        }
    }

    return S_OK;
}

HRESULT CThemeLoader::CopyDummyNonSharableDataToLive()
{
    int cDIB = _rgDIBDataArray.size();

    auto hdr = (NONSHARABLEDATAHDR*)VirtualAlloc(
        _LoadingThemeFile.NonSharableDataHeader(),
        sizeof(NONSHARABLEDATAHDR) + cDIB * sizeof(void*),
        MEM_COMMIT,
        PAGE_READWRITE);
    if (!hdr)
        return E_OUTOFMEMORY;

    hdr->cBitmaps = cDIB;
    hdr->iBitmapsOffset = sizeof(NONSHARABLEDATAHDR);
    hdr->dwFlags = 0;
    hdr->iLoadId = 0;
    if ((BYTE*)(hdr + 1) < ((BYTE*)(hdr + 1) + 8 * cDIB))
        memset(hdr + 1, 0, 8 * (((8 * cDIB - 1) / 8) + 1));

    hdr->dwFlags |= 1;

    return S_OK;
}

HRESULT CThemeLoader::CreateReuseSection(
    wchar_t const* pszSharableSectionName, void** phReuseSection)
{
    HRESULT hr = S_OK;

    int v3 = _rgDIBDataArray.size();
    DWORD maxSize = sizeof(REUSEDATAHDR) + sizeof(DIBREUSEDATA) * v3;
    if ((8 * (BYTE)v3 + 0x14) & 3)
        maxSize = (maxSize & 0xFFFFFFFC) + 4;

    for (auto& dibData : _rgDIBDataArray) {
        if (dibData.pDIBBits) {
            dibData.dibReuseData.iDIBBitsOffset = maxSize;
            maxSize += sizeof(int) * dibData.dibReuseData.iWidth * dibData.dibReuseData.iHeight;
        } else {
            dibData.dibReuseData.iDIBBitsOffset = -1;
        }
    }

    REUSEDATAHDR* reuseDataHdr = nullptr;

    *phReuseSection = CreateFileMappingW(FileHandle::InvalidHandle(), nullptr,
                                         PAGE_READWRITE, 0, maxSize, nullptr);
    if (!*phReuseSection)
        hr = MakeErrorLast();

    if (SUCCEEDED(hr)) {
        reuseDataHdr = (REUSEDATAHDR*)MapViewOfFile(*phReuseSection, FILE_MAP_WRITE, 0, 0, 0);
        if (!reuseDataHdr)
            hr = MakeErrorLast();
    }

    if (SUCCEEDED(hr)) {
        reuseDataHdr->iDIBReuseRecordsCount = _rgDIBDataArray.size();
        reuseDataHdr->iDIBReuseRecordsOffset = sizeof(REUSEDATAHDR);
        reuseDataHdr->dwTotalLength = maxSize;

        if (pszSharableSectionName)
            hr = StringCchCopyW(reuseDataHdr->szSharableSectionName, 260, pszSharableSectionName);

        if (SUCCEEDED(hr)) {
            auto reuseIt = (DIBREUSEDATA*)((BYTE*)reuseDataHdr + reuseDataHdr->iDIBReuseRecordsOffset);
            for (auto const& dibData : _rgDIBDataArray) {
                *reuseIt = dibData.dibReuseData;
                if (dibData.pDIBBits)
                    memcpy_s(
                    (BYTE*)reuseDataHdr + reuseIt->iDIBBitsOffset,
                        (int)(maxSize - reuseIt->iDIBBitsOffset),
                        dibData.pDIBBits,
                        4 * reuseIt->iWidth * reuseIt->iHeight);

                ++reuseIt;
            }
        }
    }

    if (reuseDataHdr)
        UnmapViewOfFile(reuseDataHdr);

    if (FAILED(hr) && *phReuseSection) {
        CloseHandle(*phReuseSection);
        *phReuseSection = nullptr;
    }

    return hr;
}

static HRESULT GenerateNonSharableData(HANDLE hReuseSection, void* pNonSharableData)
{
    FileViewHandle view{MapViewOfFile(hReuseSection, FILE_MAP_READ, 0, 0, 0)};

    auto const reuseDataHdr = static_cast<REUSEDATAHDR*>(view.Get());
    if (!reuseDataHdr)
        return MakeErrorLast();

    auto const nonSharableDataHdr = static_cast<NONSHARABLEDATAHDR*>(pNonSharableData);
    nonSharableDataHdr->dwFlags = 0;
    nonSharableDataHdr->iLoadId = 0;
    nonSharableDataHdr->cBitmaps = reuseDataHdr->iDIBReuseRecordsCount;
    nonSharableDataHdr->iBitmapsOffset = sizeof(NONSHARABLEDATAHDR);

    if (!GetWindowDC(nullptr))
        return E_FAIL;

    BITMAPHEADER bmi;
    bmi.bmih.biWidth = 0;
    bmi.bmih.biHeight = 0;
    bmi.bmih.biSizeImage = 0;
    bmi.bmih.biCompression = BI_BITFIELDS;
    bmi.bmih.biClrUsed = 3;
    bmi.bmih.biSize = sizeof(bmi.bmih);
    bmi.bmih.biPlanes = 1;
    bmi.bmih.biBitCount = 32;
    bmi.masks[0] = 0xFF0000;
    bmi.masks[1] = 0x00FF00;
    bmi.masks[2] = 0x0000FF;

    HBITMAP* dstBitmap = (HBITMAP*)((BYTE*)nonSharableDataHdr + nonSharableDataHdr->iBitmapsOffset);
    HBITMAP* const dstBitmapEnd = dstBitmap + reuseDataHdr->iDIBReuseRecordsCount;
    DIBREUSEDATA* dibReuseRec = (DIBREUSEDATA*)((BYTE*)reuseDataHdr + reuseDataHdr->iDIBReuseRecordsOffset);

    for (; dstBitmap < dstBitmapEnd; ++dstBitmap, ++dibReuseRec) {
        if (dibReuseRec->iDIBBitsOffset == -1) {
            *dstBitmap = nullptr;
            continue;
        }

        bmi.bmih.biWidth = dibReuseRec->iWidth;
        bmi.bmih.biHeight = dibReuseRec->iHeight;
        bmi.bmih.biSizeImage = 4 * dibReuseRec->iWidth * dibReuseRec->iHeight;

        void* pvBits = nullptr;
        *dstBitmap = CreateDIBSection(nullptr,
                                      reinterpret_cast<BITMAPINFO const*>(&bmi),
                                      DIB_RGB_COLORS, &pvBits, hReuseSection,
                                      dibReuseRec->iDIBBitsOffset);
        if (!*dstBitmap)
            return E_OUTOFMEMORY;
    }

    nonSharableDataHdr->dwFlags |= 7;
    return S_OK;
}

HRESULT CThemeLoader::CopyNonSharableDataToLive(HANDLE hReuseSection)
{
    void* ptr = VirtualAlloc(
        _LoadingThemeFile.NonSharableDataHeader(),
        8 * _rgDIBDataArray.size() + 16,
        MEM_COMMIT, PAGE_READWRITE);

    if (!ptr)
        return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);

    return GenerateNonSharableData(hReuseSection, ptr);
}

HRESULT CThemeLoader::PackAndLoadTheme(
    HANDLE hFile, wchar_t const* pszThemeName, wchar_t const* pszColorParam,
    wchar_t const* pszSizeParam, unsigned cbMaxDesiredSharableSectionSize,
    wchar_t* pszSharableSectionName, unsigned cchSharableSectionName,
    wchar_t* pszNonSharableSectionName, unsigned cchNonSharableSectionName,
    HANDLE* phReuseSection,
    PFNALLOCSECTIONS pfnAllocSections)
{
    ENSURE_HR(PackMetrics());

    if (pfnAllocSections) {
        ENSURE_HR(pfnAllocSections(
            &_LoadingThemeFile,
            pszSharableSectionName,
            cchSharableSectionName,
            cbMaxDesiredSharableSectionSize,
            pszNonSharableSectionName,
            cchNonSharableSectionName,
            8 * _rgDIBDataArray.size() + 16,
            1));
    } else {
        ENSURE_HR(_LoadingThemeFile.CreateFileW(
            pszSharableSectionName,
            cchSharableSectionName,
            cbMaxDesiredSharableSectionSize,
            pszNonSharableSectionName,
            cchNonSharableSectionName,
            8 * _rgDIBDataArray.size() + 16,
            0));
    }

    if (phReuseSection) {
        ENSURE_HR(CreateReuseSection(pszSharableSectionName, phReuseSection));
        ENSURE_HR(CopyNonSharableDataToLive(*phReuseSection));
    } else {
        ENSURE_HR(CopyDummyNonSharableDataToLive());
    }

    ENSURE_HR(CopyLocalThemeToLive(
        hFile,
        cbMaxDesiredSharableSectionSize,
        pszThemeName,
        pszColorParam,
        pszSizeParam));

    return S_OK;
}

HRESULT CThemeLoader::CopyLocalThemeToLive(
    void* hFile, int iTotalLength, wchar_t const* pszThemeName,
    wchar_t const* pszColorParam, wchar_t const* pszSizeParam)
{
    auto const themeHdrSize = Align8(sizeof(THEMEHDR));
    auto const themeHdr = (THEMEHDR*)VirtualAlloc(
        _LoadingThemeFile.ThemeHeader(), themeHdrSize, MEM_COMMIT, PAGE_READWRITE);
    if (!themeHdr)
        return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);

    _hdr = themeHdr;
    _iGlobalsOffset = -1;
    _iSysMetricsOffset = -1;

    _hdr->dwTotalLength = iTotalLength;
    *(uint64_t*)&_hdr->szSignature = 0x4D48544E49474542;
    _hdr->dwVersion = 65543;
    _hdr->iDllNameOffset = 0;
    _hdr->iColorParamOffset = 0;
    _hdr->iSizeParamOffset = 0;
    _hdr->dwLangID = _wCurrentLangID;
    _hdr->iLoadDPI = GetScreenDpi();
    _hdr->dwLoadDPIs = g_DpiInfo._nDpiPlateausCurrentlyPresent;
    _hdr->iLoadPPI = GetScreenPpi();
    _hdr->iGlobalsOffset = 0;
    _hdr->iGlobalsTextObjOffset = 0;
    _hdr->iGlobalsDrawObjOffset = 0;
    _hdr->iFontsOffset = 0;
    _hdr->cFonts = _fontTable.size();
    _hdr->ftModifTimeStamp = FILETIME();
    GetFileTime(hFile, nullptr, nullptr, &_hdr->ftModifTimeStamp);

    MIXEDPTRS u;
    u.pb = (BYTE*)themeHdr + themeHdrSize;

    _hdr->iStringsOffset = (uintptr_t)u.pb - (uintptr_t)(_LoadingThemeFile.ThemeHeader());

    BYTE* stringsBegin = u.pb;
    ENSURE_HR(EmitString(&u, pszThemeName, 260, &_hdr->iDllNameOffset));

    if (int len = lstrlenW(pszColorParam))
        ENSURE_HR(EmitString(&u, pszColorParam, len, &_hdr->iColorParamOffset));
    if (int len = lstrlenW(pszSizeParam))
        ENSURE_HR(EmitString(&u, pszSizeParam, len, &_hdr->iSizeParamOffset));

    for (APPCLASSLOCAL& localCls : _LocalIndexes) {
        if (size_t len = localCls.csAppName.length()) {
            ENSURE_HR(EmitString(&u, localCls.csAppName.c_str(), len,
                                 &localCls.AppClassInfo.iAppNameIndex));
        } else {
            localCls.AppClassInfo.iAppNameIndex = 0;
        }

        if (size_t len = localCls.csClassName.length()) {
            ENSURE_HR(EmitString(&u, localCls.csClassName.c_str(), len,
                                 &localCls.AppClassInfo.iClassNameIndex));
        } else {
            localCls.AppClassInfo.iClassNameIndex = 0;
        }
    }

    {
        int* pStringOffsets = (int*)_LoadThemeMetrics.iStringOffsets;
        for (std::wstring const& str : _LoadThemeMetrics.wsStrings) {
            if (size_t len = str.length())
                ENSURE_HR(EmitString(&u, str.c_str(), len, pStringOffsets));
            else
                *pStringOffsets = 0;
            ++pStringOffsets;
        }
    }

    _hdr->iStringsLength = (uintptr_t)u.pb - (uintptr_t)stringsBegin;

    _hdr->iSectionIndexOffset = (uintptr_t)u.pb - (uintptr_t)_LoadingThemeFile.ThemeHeader();
    _hdr->iSectionIndexLength = sizeof(APPCLASSLIVE) * _LocalIndexes.size();
    ENSURE_HR(AllocateThemeFileBytes(u.pb, _hdr->iSectionIndexLength));

    auto liveClasses = (APPCLASSLIVE*)u.pb;
    u.pb += _hdr->iSectionIndexLength;
    for (size_t i = 0; i < _LocalIndexes.size(); ++i) {
        APPCLASSLOCAL& pac = _LocalIndexes[i];
        APPCLASSLIVE& liveCls = liveClasses[i];
        liveCls.AppClassInfo = pac.AppClassInfo;
        liveCls.iIndex = (uintptr_t)u.pb - (uintptr_t)_LoadingThemeFile.ThemeHeader();

        if (!AsciiStrCmpI(pac.csClassName.c_str(), L"globals"))
            _iGlobalsOffset = liveCls.iIndex;

        if (_rgBaseClassIds[i] == -1)
            liveCls.iBaseClassIndex = _iGlobalsOffset;
        else
            liveCls.iBaseClassIndex = liveClasses[_rgBaseClassIds[i]].iIndex;

        if (!AsciiStrCmpI(pac.csClassName.c_str(), L"sysmetrics")) {
            _iSysMetricsOffset = liveCls.iIndex;
            ENSURE_HR(EmitEntryHdr(&u, TMT_THEMEMETRICS, TMT_THEMEMETRICS));
            ENSURE_HR(EmitAndCopyBlock(&u, &_LoadThemeMetrics, sizeof(THEMEMETRICS)));
            EndEntry(&u);

            ENSURE_HR(EmitEntryHdr(&u, TMT_JUMPTOPARENT, TMT_JUMPTOPARENT));
            ENSURE_HR(AllocateThemeFileBytes(u.pb, 8));
            *(int*)u.pb = -1;
            u.pb += 8;
            EndEntry(&u);
        } else {
            ENSURE_HR(CopyClassGroup(&pac, &u, &liveCls));
        }

        int currIndex = (uintptr_t)u.pb - (uintptr_t)_LoadingThemeFile.ThemeHeader();
        liveCls.iLen = currIndex - liveCls.iIndex;
    }

    {
        auto pb = u.pb;
        ENSURE_HR(EmitAndCopyBlock(&u, _fontTable.data(), sizeof(_fontTable[0]) * _fontTable.size()));
        _hdr->iFontsOffset = (uintptr_t)pb - (uintptr_t)_LoadingThemeFile.ThemeHeader();
    }

    ENSURE_HR(EmitAndCopyBlock(&u, "ENDTHEME", 8));

    _hdr->dwTotalLength = (uintptr_t)u.pb - (uintptr_t)_LoadingThemeFile.ThemeHeader();
    _hdr->iGlobalsOffset = _iGlobalsOffset;
    _hdr->iSysMetricsOffset = _iSysMetricsOffset;
    _hdr->iGlobalsTextObjOffset = _iGlobalsTextObj;
    _hdr->iGlobalsDrawObjOffset = _iGlobalsDrawObj;

    return S_OK;
}

HRESULT CThemeLoader::CopyClassGroup(APPCLASSLOCAL* pac, MIXEDPTRS* u,
                                     APPCLASSLIVE* pacl)
{
    bool fGlobalsGroup;
    HRESULT hr;
    int* partJumpTable;
    int iPartZeroIndex;
    int screenDpi;
    int v21;
    BYTE* v26;
    CRenderObj* pRender;

    pRender = 0i64;
    v26 = u->pb;
    fGlobalsGroup = _iGlobalsOffset == (uintptr_t)(u->pi) - (uintptr_t)(_LoadingThemeFile.ThemeHeader());

    int cParts = pac->iMaxPartNum + 1;

    ENSURE_HR(EmitEntryHdr(u, TMT_PARTJUMPTABLE, TMT_PARTJUMPTABLE));
    ENSURE_HR(AllocateThemeFileBytes(
        u->pb, sizeof(PARTJUMPTABLEHDR) + sizeof(int) * cParts));

    auto partJumpTableHdr = (PARTJUMPTABLEHDR*)u->pb;
    partJumpTableHdr->iBaseClassIndex = pacl->iBaseClassIndex;
    partJumpTableHdr->iFirstTextObjIndex = 0;
    partJumpTableHdr->cParts = cParts;
    u->pb = (BYTE*)(partJumpTableHdr + 1);
    RegisterPtr(partJumpTableHdr);

    partJumpTable = (int*)u->pb;
    for (int i = cParts; i; --i) {
        *(int*)u->pb = -1;
        u->pb += 4;
    }
    EndEntry(u);

    iPartZeroIndex = (uintptr_t)(u->pi) - (uintptr_t)(_LoadingThemeFile.ThemeHeader());
    for (int partId = 0; partId <= pac->iMaxPartNum; ++partId)
        CopyPartGroup(pac, u, partId, partJumpTable, iPartZeroIndex, pacl->iBaseClassIndex, fGlobalsGroup);

    screenDpi = GetScreenDpi();
    hr = CRenderObj::Create(
        &_LoadingThemeFile,
        0,
        (int)v26 - (uintptr_t)(_LoadingThemeFile.ThemeHeader()),
        0,
        pacl->AppClassInfo.iClassNameIndex,
        0,
        false,
        nullptr,
        nullptr,
        screenDpi,
        false,
        0,
        &pRender);

    if (hr >= 0) {
        if (screenDpi != 96)
            pRender->_dwOtdFlags |= OTD_FORCE_RECT_SIZING;

        if (fGlobalsGroup)
            _iGlobalsDrawObj = (uintptr_t)(u->pi) - (uintptr_t)(_LoadingThemeFile.ThemeHeader());

        partJumpTableHdr->iFirstDrawObjIndex = (uintptr_t)(u->pi) - (uintptr_t)(_LoadingThemeFile.ThemeHeader());
        hr = PackDrawObjects(u, pRender, pac->iMaxPartNum, fGlobalsGroup);
        if (hr >= 0) {
            if (fGlobalsGroup)
                _iGlobalsTextObj = (uintptr_t)(u->pi) - (uintptr_t)(_LoadingThemeFile.ThemeHeader());
            partJumpTableHdr->iFirstTextObjIndex = (uintptr_t)(u->pi) - (uintptr_t)(_LoadingThemeFile.ThemeHeader());
            hr = PackTextObjects(u, pRender, pac->iMaxPartNum, fGlobalsGroup);
            if (hr >= 0)
            {
                hr = EmitEntryHdr(u, TMT_ENDOFCLASS, TMT_ENDOFCLASS);
                if (hr >= 0)
                    EndEntry(u);
            }
        }
    }

    delete pRender;
    return hr;
}

int CThemeLoader::GetPartOffset(CRenderObj* pRender, int iPartId)
{
    auto entryHdr = (ENTRYHDR*)pRender->_pbSectionData;
    if (entryHdr->usTypeNum != TMT_PARTJUMPTABLE)
        return -1;

    auto jumpTableHdr = (PARTJUMPTABLEHDR*)(entryHdr + 1);
    if (iPartId >= jumpTableHdr->cParts)
        return -1;

    auto jumpTable = (int*)(jumpTableHdr + 1);
    return jumpTable[iPartId];
}

BOOL CThemeLoader::KeyDrawPropertyFound(int iStateDataOffset)
{
    auto ptr = (BYTE*)_LoadingThemeFile.ThemeHeader() + iStateDataOffset;
    for (;;) {
        auto hdr = (ENTRYHDR*)ptr;
        if (hdr->usTypeNum == TMT_JUMPTOPARENT)
            break;

        if (CBorderFill::KeyProperty(hdr->usTypeNum))
            return TRUE;
        if (CImageFile::KeyProperty(hdr->usTypeNum))
            return TRUE;

        ptr += sizeof(ENTRYHDR) + hdr->dwDataLen;
    }

    return FALSE;
}

HRESULT CThemeLoader::PackDrawObjects(
    MIXEDPTRS* uOut, CRenderObj* pRender, int iMaxPart, bool fGlobals)
{
    for (int partId = 0; partId <= iMaxPart; ++partId) {
        int offset = GetPartOffset(pRender, partId);
        if (offset == -1)
            continue;

        auto entryHdr = (ENTRYHDR*)((BYTE*)_LoadingThemeFile.ThemeHeader() + offset);
        if (entryHdr->usTypeNum == TMT_STATEJUMPTABLE) {
            auto jumpTableHdr = (STATEJUMPTABLEHDR*)(entryHdr + 1);
            auto jumpTable = (int*)(jumpTableHdr + 1);

            for (int stateId = 0; stateId <= jumpTableHdr->cStates - 1; ++stateId) {
                int so = jumpTable[stateId];
                if (so != -1 && (fGlobals || KeyDrawPropertyFound(so))) {
                    ENSURE_HR(PackDrawObject(uOut, pRender, partId, stateId));
                    fGlobals = 0;
                }
            }
        } else {
            if (fGlobals || KeyDrawPropertyFound(offset))
                ENSURE_HR(PackDrawObject(uOut, pRender, partId, 0));
        }
    }

    return S_OK;
}

HRESULT CThemeLoader::CopyPartGroup(
    APPCLASSLOCAL* pac, MIXEDPTRS* u, int iPartId, int* piPartJumpTable,
    int iPartZeroIndex, int iBaseClassIndex, bool fGlobalsGroup)
{
    if (pac->PartStateIndexes.empty())
        return S_OK;

    int iMaxStateId = -1;
    for (PART_STATE_INDEX const& psi : pac->PartStateIndexes) {
        if (psi.iPartId == iPartId && psi.iStateId > iMaxStateId)
            iMaxStateId = psi.iStateId;
    }

    if (iMaxStateId < 0)
        return S_OK;

    HRESULT hr = 0;
    if (piPartJumpTable) {
        piPartJumpTable[iPartId] = (uintptr_t)(u->pi) - (uintptr_t)(_LoadingThemeFile.ThemeHeader());
        if (piPartJumpTable[iPartId] == 0) { // FIXME: Remove
            piPartJumpTable[iPartId] = 0;
        }
    }

    int* stateJumpTable = nullptr;
    if (iMaxStateId > 0) {
        int cStates = iMaxStateId + 1;
        ENSURE_HR(EmitEntryHdr(u, TMT_STATEJUMPTABLE, TMT_STATEJUMPTABLE));
        ENSURE_HR(AllocateThemeFileBytes(
            u->pb, sizeof(STATEJUMPTABLEHDR) + sizeof(int) * cStates));

        auto stateJumpTableHdr = (STATEJUMPTABLEHDR*)u->pb;
        stateJumpTableHdr->cStates = cStates;
        RegisterPtr(stateJumpTableHdr);

        u->pb = (BYTE*)(stateJumpTableHdr + 1);
        stateJumpTable = (int*)u->pb;

        for (int i = 0; i < cStates; ++i)
            stateJumpTable[i] = -1;

        u->pb += sizeof(int) * cStates;

        EndEntry(u);
    }

    int v24 = (uintptr_t)(u->pi) - (uintptr_t)(_LoadingThemeFile.ThemeHeader());
    for (int iState = 0; iState <= iMaxStateId; ++iState) {
        for (auto const& psi : pac->PartStateIndexes) {
            if (psi.iPartId != iPartId || psi.iStateId != iState)
                continue;

            if (stateJumpTable)
                stateJumpTable[iState] = (uintptr_t)(u->pi) - (uintptr_t)(_LoadingThemeFile.ThemeHeader());

            int vv;
            if (iState)
                vv = v24;
            else if (fGlobalsGroup)
                vv = -1;
            else
                vv = iPartId ? iPartZeroIndex : iBaseClassIndex;

            BYTE* block = &_pbLocalData[psi.iIndex];
            *(int*)(_pbLocalData + psi.iIndex + psi.iLen - 8) = vv;

            auto begin = (ENTRYHDR const*)u->pb;
            auto end = Advance(begin, psi.iLen);

            hr = EmitAndCopyBlock(u, block, psi.iLen);

            for (auto p = begin; p < end;) {
                RegisterPtr(p);
                p = Advance(p, sizeof(ENTRYHDR) + p->dwDataLen);
            }
        }
    }

    return hr;
}

HRESULT CThemeLoader::MakeStockObject(CRenderObj* pRender, DIBINFO* pdi)
{
    if (!_fGlobalTheme)
        return S_FALSE;

    TMBITMAPHEADER* hdr = pRender->GetBitmapHeader(pdi->iDibOffset);
    if (!hdr)
        return S_FALSE;

    HBITMAP hbmp = pRender->BitmapIndexToHandle(hdr->iBitmapIndex);
    if (!hbmp)
        return S_FALSE;

    HRESULT v4;
    HBITMAP v10;
    v4 = 1;
    //LODWORD(v10) = SetBitmapAttributes(hbmp, (unsigned)v4);
    //if (v10) {
    pRender->SetBitmapHandle(hdr->iBitmapIndex, hbmp);
    //    return S_OK;
    //} else {
    //    DeleteObject(hbmp);
    //    return E_FAIL;
    //}
    return S_OK;
}

class CBitmapPixels
{
public:
    HRESULT OpenBitmap(HDC hdc, HBITMAP bitmap, bool fForceRGB32,
                       unsigned** pPixels, int* piWidth, int* piHeight,
                       int* piBytesPerPixel, int* piBytesPerRow,
                       int* piPreviousBytesPerPixel, unsigned cbBytesBefore);
    void CloseBitmap(HDC hdc, HBITMAP hBitmap);

private:
    malloc_ptr<BYTE[]> _buffer;
    BITMAPINFOHEADER* _hdrBitmap;
    int _iWidth;
    int _iHeight;
};

HRESULT CBitmapPixels::OpenBitmap(HDC hdc, HBITMAP bitmap, bool fForceRGB32,
                                  unsigned** pPixels, int* piWidth,
                                  int* piHeight, int* piBytesPerPixel,
                                  int* piBytesPerRow,
                                  int* piPreviousBytesPerPixel,
                                  unsigned cbBytesBefore)
{
    if (!pPixels)
        return E_INVALIDARG;

    OptionalDC screenDC{hdc};
    if (!screenDC)
        return MakeErrorLast();

    BITMAP bminfo;
    GetObjectW(bitmap, sizeof(BITMAP), &bminfo);

    _iWidth = bminfo.bmWidth;
    _iHeight = bminfo.bmHeight;

    unsigned bytesPerRow;
    if (FAILED(UIntMult(4u, bminfo.bmWidth, &bytesPerRow)) || bytesPerRow > 0x7FFFFFFC)
        return E_OUTOFMEMORY;

    bytesPerRow = AlignPower2<4>(bytesPerRow);
    unsigned cbPixels;
    if (FAILED(UIntMult(AlignPower2<4>(bytesPerRow), bminfo.bmHeight, &cbPixels)) ||
        FAILED(UIntAdd(cbPixels, sizeof(BITMAPINFOHEADER) + 100, &cbPixels)))
        return E_OUTOFMEMORY;

    _buffer = make_unique_malloc<BYTE[]>(cbPixels);
    if (!_buffer)
        return E_OUTOFMEMORY;

    _hdrBitmap = reinterpret_cast<BITMAPINFOHEADER*>(_buffer.get());
    fill_zero(*_hdrBitmap);
    _hdrBitmap->biSize = sizeof(BITMAPINFOHEADER);
    _hdrBitmap->biWidth = _iWidth;
    _hdrBitmap->biHeight = _iHeight;
    _hdrBitmap->biPlanes = 1;
    _hdrBitmap->biBitCount = 32;
    _hdrBitmap->biCompression = 0;

    if (!GetDIBits(screenDC, bitmap, 0, _iHeight, GetBitmapBits(_hdrBitmap),
                   reinterpret_cast<LPBITMAPINFO>(_hdrBitmap), DIB_RGB_COLORS))
        return E_FAIL;

    *pPixels = reinterpret_cast<unsigned*>(GetBitmapBits(_hdrBitmap));

    if (piWidth)
        *piWidth = _iWidth;
    if (piHeight)
        *piHeight = _iHeight;
    if (piBytesPerPixel)
        *piBytesPerPixel = 4;
    if (piBytesPerRow)
        *piBytesPerRow = bytesPerRow;

    return S_OK;
}

void CBitmapPixels::CloseBitmap(HDC hdc, HBITMAP hBitmap)
{
    if (!_hdrBitmap || !_buffer)
        return;

    if (hBitmap) {
        HDC screenDC = GetWindowDC(nullptr);
        SetDIBits(
            screenDC,
            hBitmap,
            0,
            _iHeight,
            (BYTE*)_hdrBitmap + 4 * _hdrBitmap->biClrUsed + _hdrBitmap->biSize,
            (const BITMAPINFO *)_hdrBitmap,
            0);
        if (screenDC)
            ReleaseDC(nullptr, screenDC);
    }

    _buffer.reset();
    _hdrBitmap = nullptr;
}

HRESULT CThemeLoader::PackImageFileInfo(
    DIBINFO* pdi, CImageFile* pImageObj, MIXEDPTRS* u, CRenderObj* pRender,
    int iPartId, int iStateId)
{
    HRESULT hr;
    signed __int64 v11;
    __int64 v12;
    signed __int64 v14;
    TMBITMAPHEADER* v16;
    HBITMAP v22;
    int v23;
    signed __int64 v24;
    int v28;
    void* v29;
    int v30;
    int v31;
    HBITMAP v32;
    unsigned* v33;
    signed __int64 v34;
    int iPartIda;

    hr = 0;
    if (iStateId != 0 || !pdi->fPartiallyTransparent || pdi->iDibOffset <= 0)
        return hr;
    //if (pdi->iDibOffset == 56864) // FIXME
    //    return hr;

    v11 = pImageObj->_iImageCount;
    pdi->iRgnListOffset = (uintptr_t)(u->pi) - (uintptr_t)(_LoadingThemeFile.ThemeHeader()) + 8;

    hr = EmitEntryHdr(u, TMT_RGNLIST, TMT_RGNLIST);
    if (hr < 0) {
        EndEntry(u);
        return hr;
    }

    v12 = Align8(4 * (pImageObj->_iImageCount + 1));
    hr = AllocateThemeFileBytes(u->pb, v12 + 8);
    if (hr < 0) {
        EndEntry(u);
        return hr;
    }

    *u->pb = pImageObj->_iImageCount + 1;
    v14 = (signed __int64)(u->pb + 8);
    v34 = (signed __int64)(u->pb + 8);
    memset(u->pb + 8, 0, (unsigned)v12);

    u->pb = (BYTE*)(v14 + v12);

    v32 = 0;
    v28 = 0;
    if (pImageObj->_iImageCount >= 0) {
        if (pdi->fPartiallyTransparent) {
            if (pRender->_pbSharableData && pdi->iDibOffset > 0) {
                v16 = pRender->GetBitmapHeader(pdi->iDibOffset);
                v28 = pRender->_phBitmapsArray[v16->iBitmapIndex].hBitmap != nullptr;
            }

            hr = pRender->ExternalGetBitmap(nullptr, pdi->iDibOffset, GBF_DIRECT, &v32);
            v22 = v32;
            if (hr >= 0) {
                CBitmapPixels pixels;
                hr = pixels.OpenBitmap(
                    nullptr,
                    v32,
                    0,
                    &v33,
                    &v31,
                    &v30,
                    nullptr,
                    nullptr,
                    nullptr,
                    0);
                if (hr >= 0) {
                    v23 = 0;
                    v29 = 0;
                    iPartIda = 0;
                    if (v11 >= 0) {
                        v24 = (signed __int64)v29;
                        do {
                            RGNDATA* rgnData = nullptr;
                            int rgnDataLen;
                            hr = pImageObj->BuildRgnData(
                                v33,
                                v31,
                                v30,
                                pdi,
                                pRender,
                                v23,
                                &rgnData,
                                &rgnDataLen);
                            if (hr >= 0) {
                                if (rgnDataLen) {
                                    *(DWORD *)(v34 + 4 * v24) = (uintptr_t)(u->pi) - (uintptr_t)(_LoadingThemeFile.ThemeHeader());
                                    hr = EmitEntryHdr(u, TMT_RGNDATA, TMT_RGNDATA);
                                    if (hr >= 0) {
                                        hr = EmitAndCopyBlock(u, rgnData, rgnDataLen);
                                        EndEntry(u);
                                    }
                                }
                            }
                            free(rgnData);
                            ++v24;
                            v23 = iPartIda++ + 1;
                        } while (v24 <= v11);
                        v22 = v32;
                    }
                }
            }
            if (v22 && !v28)
                DeleteObject(v22);
        }
    }

    EndEntry(u);
    return hr;
}

} // namespace uxtheme
