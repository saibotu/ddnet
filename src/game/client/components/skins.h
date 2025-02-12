/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_SKINS_H
#define GAME_CLIENT_COMPONENTS_SKINS_H
#include <base/color.h>
#include <base/tl/sorted_array.h>
#include <base/vmath.h>
#include <engine/shared/http.h>
#include <game/client/component.h>
#include <game/client/skin.h>

class CSkins : public CComponent
{
public:
	class CGetPngFile : public CHttpRequest
	{
		CSkins *m_pSkins;

	protected:
		virtual int OnCompletion(int State) override;

	public:
		CGetPngFile(CSkins *pSkins, const char *pUrl, IStorage *pStorage, const char *pDest);
		CImageInfo m_Info;
	};

	struct CDownloadSkin
	{
		std::shared_ptr<CSkins::CGetPngFile> m_pTask;
		char m_aPath[IO_MAX_PATH_LENGTH];
		char m_aName[24];

		bool operator<(const CDownloadSkin &Other) const { return str_comp(m_aName, Other.m_aName) < 0; }
		bool operator<(const char *pOther) const { return str_comp(m_aName, pOther) < 0; }
		bool operator==(const char *pOther) const { return !str_comp(m_aName, pOther); }
	};

	typedef std::function<void(int)> TSkinLoadedCBFunc;

	virtual int Sizeof() const override { return sizeof(*this); }
	void OnInit() override;

	void Refresh(TSkinLoadedCBFunc &&SkinLoadedFunc);
	int Num();
	const CSkin *Get(int Index);
	int Find(const char *pName);

private:
	sorted_array<CSkin> m_aSkins;
	sorted_array<CDownloadSkin> m_aDownloadSkins;
	char m_EventSkinPrefix[24];

	bool LoadSkinPNG(CImageInfo &Info, const char *pName, const char *pPath, int DirType);
	int LoadSkin(const char *pName, const char *pPath, int DirType);
	int LoadSkin(const char *pName, CImageInfo &Info);
	int FindImpl(const char *pName);
	static int SkinScan(const char *pName, int IsDir, int DirType, void *pUser);
};
#endif
