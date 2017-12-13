//-----------------------------------------------------------------------------
// Class:	CParaWorldAsset
// Authors:	LiXizhi
// Emails:	LiXizhi@yeah.net
// Company: ParaEngine
// Date:	2004.3.7, revised 2014.8
//				
// the asset manager class in ParaEngine.
//
// it contains sub managers for mesh, animated mesh, texture, font, sprite, database, buffer, effect, flash texture, occlusion query, archives, etc.
// it also provides the get and load functions for all above asset types. 
// All assets are reference counted and can be garbage collected on demand. All device related asset are automatically restored when device changes.
// 
// @note: each asset uses its asset file path as the internal key; it may also contain a shotcut key; however, in most cases, the shotcut key is empty. 
// shortcut key is used to get certain asset with a shorter and user friendly name. 
// 
// @function: In mesh and character file runtime remapping is implemented. In case some files are not found (possibly because of moving the directory), the engine can search the disk for possible replaceables using the filename of the missing asset file. the implementation is in ParaWorldAsset::m_bUseAssetSearch.
//  in order to enable this function, one needs to create an empty file at "temp/assetmap.txt". Check the log file for information at runtime. "temp/assetmap.txt" will be read and written at at loading and exiting time.
//-----------------------------------------------------------------------------
#include "ParaEngine.h"
#ifdef USE_DIRECTX_RENDERER
#include "resource.h"
#include <rmxftmpl.h>	// for DirectX file
#include "OcclusionQueryBank.h"
#ifdef USE_XACT_AUDIO_ENGINE
	#include "AudioEngine.h"
#endif
#endif
#include "MoviePlatform.h"
#include "SceneState.h"
#include "CharacterDB.h"
#include "DataProviderManager.h"
#include "ic/ICDBManager.h"
#include "OceanManager.h"
#include "ParaVertexBufferPool.h"
#include "AssetManifest.h"
#include "NPLRuntime.h"
#include "NPLNetClient.h"
#include "util/HttpUtility.h"
#include "FileManager.h"
#include "AsyncLoader.h"
#include "BufferPicking.h"
#include "ParaWorldAsset.h"
#include "memdebug.h"
#include "Platform/Windows/Render/D3D9/D3D9RenderDevice.h"

/**@def whether to use asset map */
// #define USE_ASSET_MAP

/**@def the asset map */
#define ASSET_MAP_FILEPATH	"temp/assetmap.txt"


/** @def define this false to disable async loading for textures by default*/
#define IS_ASYNC_LOAD	true

using namespace ParaEngine;

CParaWorldAsset* g_singleton_asset_manager;

CParaWorldAsset* CParaWorldAsset::GetSingleton()
{
	return g_singleton_asset_manager;
}
/*
* CParaWorldAsset class implementation.
**/
CParaWorldAsset::CParaWorldAsset(void)
	: m_bAsyncLoading(IS_ASYNC_LOAD)
{
	g_singleton_asset_manager = this;
	//////////////////////////////////////////////////////////////////////////
	// for file mapping. 
	{
		m_bUseAssetSearch = false;
#ifdef USE_ASSET_MAP
		CParaFile file(ASSET_MAP_FILEPATH);
		// this will be set to true when the manager class is created if there is a file called temp/assetmap.txt
		m_bUseAssetSearch = !file.isEof();

		if(m_bUseAssetSearch)
		{
			OUTPUT_LOG("NOTICE: the asset file mapping is loaded from temp/assetmap.txt \r\n At release time, one needs to delete the assetmap file to disable file mapping. \r\n\r\n");
			// file format: on each line, it is string=string, where the second string may be none. 
			char buf[2049];
			while (file.GetNextLine(buf, 2048)>0)
			{
				for (int i=0;buf[i]!='\0';++i)
				{
					if(buf[i] == '=')
					{
						buf[i] = '\0';
						m_AssetMap[buf] = &(buf[i+1]);
						//OUTPUT_LOG("%s maps to %s\r\n", buf, &(buf[i+1]));
					}
				}
			}
		}
#endif
	}
#ifdef USE_DIRECTX_RENDERER
	m_pDXFile = NULL;
	m_pOcclusionQuery = NULL;
	m_pShadowSquareVB = NULL;

	// create XFile object and register templates, if we have not done so.
	if(m_pDXFile == NULL)
	{
		CreateXFileParser(&m_pDXFile);
	}
#endif
#ifdef PARAENGINE_MOBILE
	CAsyncLoader::GetSingleton().Start(1);
#else
	CAsyncLoader::GetSingleton().Start(2);
#endif

	CreateAttributeModel();
}

CParaWorldAsset::~CParaWorldAsset(void)
{
	Cleanup();
	DeleteTempDiskTextures();
	SaveAssetFileMapping();
}

void ParaEngine::CParaWorldAsset::SaveAssetFileMapping()
{
#ifdef USE_DIRECTX_RENDERER
	if (m_bUseAssetSearch)
	{
		CParaFile file;
		if (file.OpenFile(ASSET_MAP_FILEPATH, false))
		{
			file.SetFilePointer(0, FILE_BEGIN);
			file.SetEndOfFile();
			file.SetFilePointer(0, FILE_BEGIN);
			file.WriteString("-- automatically generated by ParaEngine Asset manager.\n");
			map<string, string>::const_iterator itCur, itEnd = m_AssetMap.end();

			for (itCur = m_AssetMap.begin(); itCur != itEnd; ++itCur)
			{
				file.WriteFormated("%s=%s\n", (itCur->first).c_str(), (itCur->second).c_str());
			}
		}
	}
#endif
}

void ParaEngine::CParaWorldAsset::DeleteTempDiskTextures()
{
#ifdef USE_DIRECTX_RENDERER
	int nFileCount = 0;
	{
		CSearchResult* result = NULL;
		result = CGlobals::GetFileManager()->SearchFiles("temp/", "*.dds", "", 0, 10000);
		if (result)
		{
			int nCount = result->GetNumOfResult();
			for (int i = 0; i < nCount; ++i)
			{
				const CFileFindData* pFileDesc = result->GetItemData(i);
				if (pFileDesc)
				{
					::DeleteFile((result->GetRootPath() + result->GetItem(i)).c_str());
				}
			}
			nFileCount += nCount;
		}
		result = CGlobals::GetFileManager()->SearchFiles("temp/composeface/", "*.*", "", 0, 10000);
		if (result)
		{
			int nCount = result->GetNumOfResult();
			for (int i = 0; i < nCount; ++i)
			{
				const CFileFindData* pFileDesc = result->GetItemData(i);
				if (pFileDesc)
				{
					::DeleteFile((result->GetRootPath() + result->GetItem(i)).c_str());
				}
			}
			nFileCount += nCount;
		}
		result = CGlobals::GetFileManager()->SearchFiles("temp/composeskin/", "*.*", "", 0, 10000);
		if (result)
		{
			int nCount = result->GetNumOfResult();
			for (int i = 0; i < nCount; ++i)
			{
				const CFileFindData* pFileDesc = result->GetItemData(i);
				if (pFileDesc)
				{
					::DeleteFile((result->GetRootPath() + result->GetItem(i)).c_str());
				}
			}
			nFileCount += nCount;
		}
	}
	if (nFileCount > 0)
	{
		OUTPUT_LOG("%d temp texture file found and deleted.\n", nFileCount);
	}
#endif
}

void ParaEngine::CParaWorldAsset::CreateAttributeModel()
{
	m_attribute_models.clear();
	m_attribute_models.reserve(16);
	GetTextureManager().SetIdentifier("TextureManager");
	m_attribute_models.push_back(&GetTextureManager());

	GetFontManager().SetIdentifier("FontManager");
	m_attribute_models.push_back(&GetFontManager());

	GetDatabaseManager().SetIdentifier("DatabaseManager");
	m_attribute_models.push_back(&GetDatabaseManager());

	GetBufferPickingManager().SetIdentifier("BufferPickingManager");
	m_attribute_models.push_back(&GetBufferPickingManager());

	GetMeshManager().SetIdentifier("MeshEntityManager");
	m_attribute_models.push_back(&GetMeshManager());

	GetParaXManager().SetIdentifier("ParaXManager");
	m_attribute_models.push_back(&GetParaXManager());

	GetSequenceManager().SetIdentifier("SequenceManager");
	m_attribute_models.push_back(&GetSequenceManager());

	GetEffectManager().SetIdentifier("EffectManager");
	m_attribute_models.push_back(&GetEffectManager());

	GetVertexBufferPoolManager().SetIdentifier("VertexBufferPoolManager");
	m_attribute_models.push_back(&GetVertexBufferPoolManager());

	m_attribute_models.push_back(CGlobals::GetFileManager());

#ifdef USE_DIRECTX_RENDERER
	GetVoxelTerrainManager().SetIdentifier("VoxelTerrainManager");
	m_attribute_models.push_back(&GetVoxelTerrainManager());
#endif
}


SpriteFontAssetManager& CParaWorldAsset::GetFontManager()
{
	return SpriteFontAssetManager::GetInstance();
}

SpriteFontEntity* CParaWorldAsset::GetFont(const string& sIdentifier)
{
	return GetFontManager().GetByName(sIdentifier);
}

TextureAssetManager& CParaWorldAsset::GetTextureManager()
{
	return TextureAssetManager::GetInstance();
}

BufferPickingManager& ParaEngine::CParaWorldAsset::GetBufferPickingManager()
{
	return BufferPickingManager::GetInstance();
}

DatabaseAssetManager& CParaWorldAsset::GetDatabaseManager()
{
	return DatabaseAssetManager::GetInstance();
}

CVertexBufferPoolManager& ParaEngine::CParaWorldAsset::GetVertexBufferPoolManager()
{
	return CVertexBufferPoolManager::GetInstance();
}

DatabaseEntity* CParaWorldAsset::GetDatabase(const string& sIdentifier)
{
	return GetDatabaseManager().GetByName(sIdentifier);
}

TextureEntity* CParaWorldAsset::GetTexture(const string& sIdentifier)
{
	return GetTextureManager().GetByName(sIdentifier);
}

// TODO: find a way to release reference to the textures
TextureEntity* CParaWorldAsset::GetDefaultTexture(int nTextureID)
{
	static TextureEntity* s_default_textures[10] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
	PE_ASSERT(nTextureID < 10 && nTextureID >= 0);

	TextureEntity* tex = s_default_textures[nTextureID];
	if (tex)
	{
		return tex;
	}
	else
	{
		// create the texture if it is not assigned before. 
		char sName[2];
		sName[0] = '0' + nTextureID;
		sName[1] = '\0';
		tex = LoadTexture(sName, "Texture/whitedot.png", TextureEntity::StaticTexture);
		//tex->SetState(AssetEntity::ASSET_STATE_LOCAL);
		tex->LoadAsset();
		s_default_textures[nTextureID] = tex;
	}
	return tex;
}

TextureEntity* CParaWorldAsset::LoadTexture(const string&  sIdentifier, const string&  fileName, TextureEntity::_SurfaceType nSurfaceType)
{
	string sFileName;
	CParaFile::ToCanonicalFilePath(sFileName, fileName, false);
	CPathReplaceables::GetSingleton().DecodePath(sFileName, sFileName);

	if (fileName[0] == '<')
	{
		//////////////////////////////////////////////////////////////////////////
		// 
		// HTML renderer textures, the file is of the following format <html>[name#]initial page url[;clip size]
		// e.g. "<html>www.paraengine.com"  , "<html>No1#www.paraengine.com". 
		// where key name is "<html>" and "<html>No1", and initial URL is www.paraengine
		//
		//////////////////////////////////////////////////////////////////////////
		if ((fileName[1] == 'h') && (fileName[2] == 't') && (fileName[3] == 'm') && (fileName[4] == 'l') && (fileName[5] == '>'))
		{

			string sInitURL;
			string sKeyName;
			int nPos = (int)sFileName.find_first_of("#./\\:");
			if (nPos > 0 && sFileName[nPos] == '#')
			{
				sInitURL = sFileName.substr(nPos + 1);
				sKeyName = sFileName.substr(0, nPos);
			}
			else
			{
				sInitURL = sFileName.substr(6);
				sKeyName = "<html>";
			}
			pair<TextureEntity*, bool> res = GetTextureManager().CreateEntity(sIdentifier, sKeyName);
			if (res.first != NULL)
			{
				TextureEntity* pNewEntity = res.first;
				// contains the initial url.
				pNewEntity->SetLocalFileName(sInitURL.c_str());
				pNewEntity->SurfaceType = TextureEntity::HTMLTexture;
			}
			return res.first;
		}
		return NULL;
	}

	pair<TextureEntity*, bool> res = GetTextureManager().CreateEntity(sIdentifier, sFileName);
	if (res.second == true)
	{
		TextureEntity* pNewEntity = res.first;
		pNewEntity->SetAsyncLoad(IsAsyncLoading());
		bool bIsRemoteFile = false;
		{
			//////////////////////////////////////////////////////////////////////////
			// 
			// if the file name ends with  _a{0-9}{0-9}{0-9}.dds, it will be regarded as a texture sequence. 
			// and the nSurfaceType will be ignored and forced to TextureSequence 
			//
			//////////////////////////////////////////////////////////////////////////

			int nSize = (int)fileName.size();
			int nTotalTextureSequence = -1;

			if (nSize > 9)
			{
				if (fileName[nSize - 8] == 'a' && fileName[nSize - 9] == '_')
				{
					nTotalTextureSequence = 0;
					for (int i = 0; i < 3; ++i)
					{
						char s = fileName[nSize - 5 - i];
						if (s >= '0' && s <= '9')
						{
							nTotalTextureSequence += (int)(s - '0')*(int)pow((long double)10, i);
						}
						else
						{
							nTotalTextureSequence = -1;
							break;
						}
					}
				}
				// it is a remote file if the filename starts with "http:", or "https:" or "ftp:"
				bIsRemoteFile = ((fileName[0] == 'h' && fileName[1] == 't' && fileName[2] == 't' && fileName[3] == 'p' && (fileName[4] == ':' || fileName[5] == ':')) ||
					(fileName[0] == 'f' && fileName[1] == 't' && fileName[2] == 'p' && fileName[3] == ':'));
			}
			if (!bIsRemoteFile && nTotalTextureSequence > 0)
				nSurfaceType = TextureEntity::TextureSequence;

			//////////////////////////////////////////////////////////////////////////
			// 
			// if the file name ends with  .swf or flv, it will be regarded as a flash texture. 
			// and the nSurfaceType will be ignored and forced to FlashTexture
			//
			//////////////////////////////////////////////////////////////////////////
			if (nSize > 4)
			{
				if ((((fileName[nSize - 3] == 's') && (fileName[nSize - 2] == 'w') && (fileName[nSize - 1] == 'f'))) ||
					((fileName[nSize - 3] == 'f') && (fileName[nSize - 2] == 'l') && (fileName[nSize - 1] == 'v')))
				{
					nSurfaceType = TextureEntity::FlashTexture;
				}
			}
		}

		pNewEntity->SurfaceType = nSurfaceType;
		if (bIsRemoteFile)
		{
			// set as remote file
			pNewEntity->SetState(AssetEntity::ASSET_STATE_REMOTE);
			// remote file only applies to static texture. FlashTexture automatically support remote streaming, and TextureSequence can not be remote. 
			if (nSurfaceType == TextureEntity::StaticTexture)
			{
				// we will delay loading http texture, until it is used for the first time. 
				TextureEntity::TextureInfo* pInfo = (TextureEntity::TextureInfo*) (pNewEntity->GetTextureInfo());
				if (pInfo)
				{
					// TRICKY code LXZ: 2008.3.6: this works around a bug where an HTTP texture is used in a GUI control, where the GUI control can not correctly determine the image size. 
					// by setting negative size, the GUI control will try to retrieve the size of image the next time it is called. 
					pInfo->m_height = -1;
					pInfo->m_width = -1;
				}

				// this new version uses local Resource store. 
				string sCode = "ParaAsset.GetRemoteTexture(\"";
				sCode += sFileName;
				sCode += "\");";
				CGlobals::GetNPLRuntime()->GetMainRuntimeState()->DoString(sCode.c_str(), (int)sCode.size());
			}
			else
			{
				OUTPUT_LOG("warning: http remote texture %s must be static texture type, but we got type=%d \n", fileName.c_str(), nSurfaceType);
			}
		}
	}
	return res.first;
}

void CParaWorldAsset::RenderFrameMove(float fElapsedTime)
{
#ifdef USE_FLASH_MANAGER
	m_FlashManager.RenderFrameMove(fElapsedTime);
#endif
	GetVertexBufferPoolManager().TickCache();
}

bool CParaWorldAsset::RefreshAsset(const char* filename)
{
	string sFileName = filename;
	return (GetTextureManager().CheckRefresh(sFileName))
		|| (GetMeshManager().CheckRefresh(sFileName))
		|| (GetParaXManager().CheckRefresh(sFileName))
#ifdef USE_DIRECTX_RENDERER
		|| (m_CadModelManager.CheckRefresh(sFileName))
#endif
		;
}

bool CParaWorldAsset::DoAssetSearch(string& AssetFile,const char* searchDir)
{
	// given a source asset, it will first see if the asset already exist.
	if(CParaFile::DoesFileExist(AssetFile.c_str(), true))
	{
		return true;
	}
	// if the file does not exist, it will search if there is a mapping in the m_AssetMap, 
	map<string, string>::const_iterator iter = m_AssetMap.find(AssetFile);
	if(iter!=m_AssetMap.end())
	{
		OUTPUT_LOG("warning: asset map is used from %s to %s\r\n", AssetFile.c_str(), iter->second.c_str());
		AssetFile = iter->second;
		return true;
	}
	// if there is no valid mapping, it will search the disk using the file name and extensions and automatically generate a new mapping item,
	string sExt = CParaFile::GetFileExtension(AssetFile);
	string sFileName = CParaFile::GetFileName(AssetFile);

	// first search a strict file name mapping
	CSearchResult* result = CFileManager::GetInstance()->SearchFiles(searchDir, sFileName,"", 7, 1);
	if(result==0 || result->GetNumOfResult()==0)
	{
		// if not found, we will use a more obscure search with *. 
		// if the fileName contains _X such as tree_a_v.x, only tree is returned.
		size_t nPos = sFileName.find_first_of("_.");
		if(nPos!= string::npos)
		{
			sFileName = sFileName.substr(0, nPos);
		}
		sFileName = sFileName+"*."+sExt;
		result = CFileManager::GetInstance()->SearchFiles(searchDir, sFileName,"", 7, 1);
	}
	
	if(result!=0 && result->GetNumOfResult()>0)
	{
		string sFileName = result->GetRootPath()+result->GetItem(0);
		
		CParaFile::ToCanonicalFilePath(sFileName, sFileName,false);
		sFileName = CParaFile::GetRelativePath(sFileName, CParaFile::GetCurDirectory(CParaFile::APP_ROOT_DIR));

		OUTPUT_LOG("warning: asset map is ADDED from %s to %s\r\n", AssetFile.c_str(), sFileName.c_str());
		// add to mapping
		m_AssetMap[AssetFile] = sFileName;
		AssetFile = sFileName;
		return true;
	}
	else
	{
		return false;
	}
}

CBufferPicking* ParaEngine::CParaWorldAsset::GetBufferPick(const string& sIdentifier)
{
	return GetBufferPickingManager().GetEntity(sIdentifier);
}

CBufferPicking* ParaEngine::CParaWorldAsset::LoadBufferPick(const string& sIdentifier)
{
	CBufferPicking* entity = GetBufferPickingManager().GetEntity(sIdentifier);
	if (!entity)
	{
		pair<CBufferPicking*, bool> res = GetBufferPickingManager().CreateEntity(sIdentifier, sIdentifier);
		entity = res.first;
	}
	return entity;
}

DatabaseEntity* CParaWorldAsset::LoadDatabase(const string&  sIdentifier, const string&  fileName)
{
	string sFileName;
	CParaFile::ToCanonicalFilePath(sFileName, fileName, false);
	pair<DatabaseEntity*, bool> res = GetDatabaseManager().CreateEntity(sIdentifier, sFileName);
	if (res.second == true)
	{
		DatabaseEntity* pNewEntity = res.first;
		pNewEntity->sFileName = sFileName;
	}
	return res.first;
}


SpriteFontEntity* CParaWorldAsset::LoadGDIFont(const string&  sIdentifier, const string&  pstrFont, DWORD dwSize, bool bIsBold)
{
	char keyStr[MAX_PATH];
	const string& sFontName = SpriteFontEntity::TranslateFontName(pstrFont);
	//do not change or delete the following line, it is related to GUI 
	snprintf(keyStr, MAX_PATH, "%s;%d;%s", sFontName.c_str(), dwSize, bIsBold ? "bold" : "norm");
	pair<SpriteFontEntity*, bool> res = GetFontManager().CreateEntity(sIdentifier, keyStr);
	if (res.second == true)
	{
		SpriteFontEntity* pNewEntity = res.first;
#ifdef USE_DIRECTX_RENDERER
		((SpriteFontEntityDirectX*)pNewEntity)->TextureType = SpriteFontEntityDirectX::sprite_font_GDI;
		pNewEntity->m_nWeight = bIsBold ? FW_BOLD : FW_NORMAL;
#endif
#ifdef USE_OPENGL_RENDERER

#endif
		pNewEntity->m_nFontSize = dwSize;
		pNewEntity->m_sFontName = pstrFont;
		OUTPUT_LOG("font %s is created \n", keyStr);
	}
	return res.first;
}

//-----------------------------------------------------------------------------
// Name: InitDeviceObjects()
/// Desc: Initialize scene objects that has not yet been initialized.
/// Asset must be the first to be initialized. Otherwise, the global device object will not be valid
//-----------------------------------------------------------------------------
HRESULT CParaWorldAsset::InitDeviceObjects()
{
	//GetMeshManager().InitDeviceObjects(); // all lazy init
	//GetParaXManager().InitDeviceObjects(); // all lazy init

#ifdef USE_DIRECTX_RENDERER
	//GetTextureManager().InitDeviceObjects(); // all lazy init
#ifdef USE_FLASH_MANAGER
	m_FlashManager.InitDeviceObjects();
#endif
	m_HTMLBrowserManager.InitDeviceObjects();
	m_VoxelTerrainManager.InitDeviceObjects();
	m_D3DXSpriteManager.InitDeviceObjects();
	// m_SoundsManager.InitDeviceObjects(); // all lazy init
	m_EffectsManager.InitDeviceObjects();

	{
		if(m_pShadowSquareVB == NULL)
		{
			auto pRenderDevice = static_cast<CD3D9RenderDevice*>(CGlobals::GetRenderDevice());
			LPDIRECT3DDEVICE9 pd3dDevice = pRenderDevice->GetDirect3DDevice9();
			// Create a shadow square for rendering the stencil buffer contents
			if( FAILED(pd3dDevice->CreateVertexBuffer( 4*sizeof(SHADOWVERTEX),
											D3DUSAGE_WRITEONLY, SHADOWVERTEX::FVF,
											D3DPOOL_MANAGED, &m_pShadowSquareVB, NULL ) ) )
				return E_FAIL;
		}
	}
#endif
	GetFontManager().InitDeviceObjects();

	// signal
	OnInitDeviceObjects();
	return S_OK;
}

//-----------------------------------------------------------------------------
// Name: RestoreDeviceObjects()
// Desc: Initialize device dependent objects.
//-----------------------------------------------------------------------------
HRESULT CParaWorldAsset::RestoreDeviceObjects()
{
	GetFontManager().RestoreDeviceObjects();
	m_DynamicVBManager.RestoreDeviceObjects();
#ifdef USE_DIRECTX_RENDERER
	auto pRenderDevice = static_cast<CD3D9RenderDevice*>(CGlobals::GetRenderDevice());
	LPDIRECT3DDEVICE9 pd3dDevice = pRenderDevice->GetDirect3DDevice9();
	m_EffectsManager.RestoreDeviceObjects();
#ifdef USE_FLASH_MANAGER
	m_FlashManager.RestoreDeviceObjects();
#endif
	m_HTMLBrowserManager.RestoreDeviceObjects();
	m_VoxelTerrainManager.RestoreDeviceObjects();
	CGlobals::GetMoviePlatform()->RestoreDeviceObjects();
	{
		D3DVIEWPORT9 ViewPort;
		pd3dDevice->GetViewport(&ViewPort);
		 // Set the size of the big square shadow
		SHADOWVERTEX* v;
		FLOAT sx = (FLOAT)ViewPort.Width;
		FLOAT sy = (FLOAT)ViewPort.Height;
		m_pShadowSquareVB->Lock( 0, 0, (void**)&v, 0 );
		v[0].p = Vector4(  0, sy, 0.0f, 1.0f );
		v[1].p = Vector4(  0,  0, 0.0f, 1.0f );
		v[2].p = Vector4( sx, sy, 0.0f, 1.0f );
		v[3].p = Vector4( sx,  0, 0.0f, 1.0f );
		v[0].color = 0x7f000000;
		v[1].color = 0x7f000000;
		v[2].color = 0x7f000000;
		v[3].color = 0x7f000000;
		m_pShadowSquareVB->Unlock();
	}

	// Check to see if device supports visibility query
	if( D3DERR_NOTAVAILABLE == pd3dDevice->CreateQuery( D3DQUERYTYPE_OCCLUSION, NULL ) )
	{
		m_pOcclusionQuery = NULL;
	}
	else
	{
		pd3dDevice->CreateQuery( D3DQUERYTYPE_OCCLUSION, &m_pOcclusionQuery );
		for(int i=0;i<(int)(m_pOcclusionQueryBanks.size());++i)
		{
			LatentOcclusionQueryBank* pOcclusionQueryBank = new LatentOcclusionQueryBank(pd3dDevice);
			if(!pOcclusionQueryBank->IsValid())
				SAFE_DELETE(pOcclusionQueryBank);
			m_pOcclusionQueryBanks[i] = pOcclusionQueryBank;
		}
	}
#endif
	// signal
	OnRestoreDeviceObjects();
	return S_OK;
}

//-----------------------------------------------------------------------------
// Name: InvalidateDeviceObjects()
// Desc: Called when the device-dependent objects are about to be lost.
//-----------------------------------------------------------------------------
HRESULT CParaWorldAsset::InvalidateDeviceObjects()
{
	GetTextureManager().InvalidateDeviceObjects();
	GetFontManager().InvalidateDeviceObjects();
	GetMeshManager().InvalidateDeviceObjects();
	m_EffectsManager.InvalidateDeviceObjects();
	m_DynamicVBManager.InvalidateDeviceObjects();
#ifdef USE_DIRECTX_RENDERER
	
#ifdef USE_FLASH_MANAGER
	m_FlashManager.InvalidateDeviceObjects();
#endif
	m_HTMLBrowserManager.InvalidateDeviceObjects();
	m_VoxelTerrainManager.InvalidateDeviceObjects();
	
	m_CadModelManager.InitDeviceObjects();
	CGlobals::GetMoviePlatform()->InvalidateDeviceObjects();
	SAFE_RELEASE( m_pOcclusionQuery );
	for(int i=0;i<(int)(m_pOcclusionQueryBanks.size());++i)
	{
		SAFE_DELETE(m_pOcclusionQueryBanks[i]);
	}
#endif
	// signal
	OnInvalidateDeviceObjects();
	return S_OK;
}

//-----------------------------------------------------------------------------
// Name: DeleteDeviceObjects()
// Desc: Called when the app is exiting, or the device is being changed,
//       this function deletes any device dependent objects.
//-----------------------------------------------------------------------------
HRESULT CParaWorldAsset::DeleteDeviceObjects()
{
	HRESULT hr = S_OK;
	GetTextureManager().DeleteDeviceObjects();
	GetFontManager().DeleteDeviceObjects();

	GetParaXManager().DeleteDeviceObjects();
	GetMeshManager().DeleteDeviceObjects();

	m_EffectsManager.DeleteDeviceObjects();
	CGlobals::GetSceneState()->DeleteDeviceObjects();
#ifdef USE_DIRECTX_RENDERER
#ifdef USE_FLASH_MANAGER
	m_FlashManager.DeleteDeviceObjects();
#endif
	m_HTMLBrowserManager.DeleteDeviceObjects();
	m_VoxelTerrainManager.DeleteDeviceObjects();
	m_D3DXSpriteManager.DeleteDeviceObjects();
	
	m_CadModelManager.DeleteDeviceObjects();
	SAFE_RELEASE( m_pShadowSquareVB );
#endif
	// signal
	OnDeleteDeviceObjects();
	return hr;
}


HRESULT ParaEngine::CParaWorldAsset::RendererRecreated()
{
	GetVertexBufferPoolManager().RendererRecreated();
	m_EffectsManager.RendererRecreated();
	GetTextureManager().RendererRecreated();
	GetFontManager().RendererRecreated();
	// signal
	OnRendererRecreated();
	return S_OK;
}

//-----------------------------------------------------------------------------
// Name: Cleanup()
// Desc: Clean up memory object. Make sure that there are no objects referencing
//       any resources in the asset manager.
//-----------------------------------------------------------------------------
void CParaWorldAsset::Cleanup()
{
	CAsyncLoader::GetSingleton().Stop();
	GetParaXManager().Cleanup();
	GetMeshManager().Cleanup();
	CGlobals::GetOceanManager()->Cleanup();
	m_SequenceManager.Cleanup();
#ifdef USE_DIRECTX_RENDERER
	m_D3DXSpriteManager.Cleanup();
#ifdef USE_FLASH_MANAGER
	m_FlashManager.Cleanup();
#endif
	m_HTMLBrowserManager.Cleanup();
	m_VoxelTerrainManager.Cleanup();
	m_CadModelManager.Cleanup();
	SAFE_RELEASE(m_pDXFile); // delete X file object
#endif
	m_EffectsManager.Cleanup();
	GetFontManager().Cleanup();
	GetBufferPickingManager().Cleanup();
	// texture manager must be cleaned up last, since other resource entity may hold pointer to this one
	GetTextureManager().Cleanup(); 
	GetVertexBufferPoolManager().Cleanup();
	{
		// this cleanup order must not be changed. 
		CCharacterDB::GetInstance().CloseDB();
		CGlobals::GetDataProviderManager()->Cleanup();
		ParaInfoCenter::CICDBManager::Finalize();
		GetDatabaseManager().Cleanup();
	}
	
	OnCleanup();
}

void CParaWorldAsset::LoadAsset()
{
	GetFontManager().LoadAsset();
	GetDatabaseManager().LoadAsset();
	GetTextureManager().LoadAsset();
	GetBufferPickingManager().LoadAsset();
	GetMeshManager().LoadAsset();
	GetParaXManager().LoadAsset();
	m_EffectsManager.LoadAsset();
#ifdef USE_DIRECTX_RENDERER
	m_D3DXSpriteManager.LoadAsset();
#ifdef USE_FLASH_MANAGER
	m_FlashManager.LoadAsset();
#endif
	m_HTMLBrowserManager.LoadAsset();
	m_VoxelTerrainManager.LoadAsset();
	m_CadModelManager.LoadAsset();
#endif
}
void CParaWorldAsset::UnloadAsset()
{
	GetMeshManager().UnloadAsset();
	GetParaXManager().UnloadAsset();
	m_EffectsManager.UnloadAsset();
#ifdef USE_DIRECTX_RENDERER
#ifdef USE_FLASH_MANAGER
	m_FlashManager.UnloadAsset();
#endif
	m_HTMLBrowserManager.UnloadAsset();
	m_VoxelTerrainManager.UnloadAsset();
	m_CadModelManager.UnloadAsset();
#endif
	GetFontManager().UnloadAsset();
	GetDatabaseManager().UnloadAsset();
	GetBufferPickingManager().UnloadAsset();
	GetTextureManager().UnloadAsset();
}
void CParaWorldAsset::GarbageCollectAll()
{
	GetMeshManager().GarbageCollectAll();
	GetParaXManager().GarbageCollectAll();
	m_EffectsManager.GarbageCollectAll();
#ifdef USE_DIRECTX_RENDERER
	m_D3DXSpriteManager.GarbageCollectAll();
	m_SequenceManager.GarbageCollectAll();
	m_HTMLBrowserManager.GarbageCollectAll();
	m_VoxelTerrainManager.GarbageCollectAll();
	m_CadModelManager.GarbageCollectAll();
#endif
	GetFontManager().GarbageCollectAll();
	GetDatabaseManager().GarbageCollectAll();
	GetBufferPickingManager().GarbageCollectAll();
	GetTextureManager().GarbageCollectAll();
}

bool CParaWorldAsset::UnloadAssetByKeyName(const string& keyname)
{
	string sFileExt = CParaFile::GetFileExtension(keyname);
	if(sFileExt == "dds" || sFileExt == "png")
	{
		TextureEntity* pEntity = (TextureEntity*) GetTextureManager().get(keyname);
		if(pEntity && (pEntity->GetState()==AssetEntity::ASSET_STATE_FAILED_TO_LOAD || pEntity->IsLoaded()))
		{
			pEntity->UnloadAsset();
			pEntity->SetLocalFileName("");
			if(pEntity->GetState()==AssetEntity::ASSET_STATE_FAILED_TO_LOAD)
				pEntity->SetState(AssetEntity::ASSET_STATE_NORMAL);
			return true;
		}
	}

	else if(sFileExt == "x" || sFileExt == "xml")
	{
		{
			MeshEntity* pEntity = (MeshEntity*) GetMeshManager().get(keyname);
			if(pEntity && pEntity->IsLoaded())
			{
				pEntity->UnloadAsset();
				pEntity->SetLocalFileName("");
				return true;
			}
		}
		{
			ParaXEntity* pEntity = (ParaXEntity*) GetParaXManager().get(keyname);
			if(pEntity && pEntity->IsLoaded())
			{
				pEntity->UnloadAsset();
				pEntity->SetLocalFileName("");
				return true;
			}
		}
	}
#ifdef USE_DIRECTX_RENDERER
	else if(sFileExt == "iges")
	{
		CadModel* pCadModel = (CadModel*) m_CadModelManager.get(keyname);
		if(pCadModel && pCadModel->IsLoaded())
		{
			pCadModel->UnloadAsset();
			pCadModel->SetLocalFileName("");
			return true;
		}
	}
#endif
	return false;
}


ParaXEntity* CParaWorldAsset::GetParaX(const string& sIdentifier)
{
	return GetParaXManager().GetByName(sIdentifier);
}


//--------------------------------------------------------------------
// Desc: Get Mesh Object by its identifier
// Params: 
//--------------------------------------------------------------------
MeshEntity* CParaWorldAsset::GetMesh(const string& sIdentifier)
{
	return GetMeshManager().GetByName(sIdentifier);
}

//--------------------------------------------------------------------
// Desc: Load new template into memory, template with the same identifier
//		will not be created twice. 
//
// Params: sMeshFileName the identifier string
//--------------------------------------------------------------------
MeshEntity* CParaWorldAsset::LoadMesh(const string&  sIdentifier, const string&  fileName)
{
	string sFileName;
	CParaFile::ToCanonicalFilePath(sFileName, fileName, false);
	if (m_bUseAssetSearch)
		DoAssetSearch(sFileName, CParaFile::GetCurDirectory(CParaFile::APP_MODEL_DIR).c_str());
	pair<MeshEntity*, bool> res = GetMeshManager().CreateEntity(sIdentifier, sFileName);
	if (res.second == true)
	{
		MeshEntity* pNewEntity = res.first;
		pNewEntity->Init();
	}
	return res.first;
}

SequenceEntity* CParaWorldAsset::LoadSequence(const string& sName)
{
	pair<SequenceEntity*, bool> res = m_SequenceManager.CreateEntity("", sName);
	if (res.second == true)
	{
		SequenceEntity* pNewEntity = res.first;
	}
	return res.first;
}

ParaXEntity* CParaWorldAsset::LoadParaX(const string&  sIdentifier, const string&  fileName)
{
	string sFileName;
	CParaFile::ToCanonicalFilePath(sFileName, fileName, false);
	if (m_bUseAssetSearch)
		DoAssetSearch(sFileName, CParaFile::GetCurDirectory(CParaFile::APP_CHARACTER_DIR).c_str());
	pair<ParaXEntity*, bool> res = GetParaXManager().CreateEntity(sIdentifier, sFileName);
	if (res.second == true)
	{
		ParaXEntity* pNewEntity = res.first;
		pNewEntity->Init(sFileName.c_str());
	}
	return res.first;
}

ParaXEntity* CParaWorldAsset::LoadParaXByID(int nAssetID)
{
	// TODO: just for testing,
	// read from client database, instead.

	static const char models_[][50] =
	{
		"character/particles/white_missile.x",	// looped missile
		"character/particles/LevelUp.x",
		"character/particles/summonNew.x",
		"character/particles/ring.x",
	};
	nAssetID = nAssetID % 4;
	return LoadParaX("", models_[nAssetID]);
}

MeshEntityManager& ParaEngine::CParaWorldAsset::GetMeshManager()
{
	return MeshEntityManager::GetInstance();
}

ParaXEntityManager& ParaEngine::CParaWorldAsset::GetParaXManager()
{
	return ParaXEntityManager::GetInstance();
}

DynamicVertexBufferEntity* ParaEngine::CParaWorldAsset::GetDynamicBuffer(DynamicVBAssetType nBufferType)
{
	return m_DynamicVBManager.GetDynamicBuffer(nBufferType);
}

IAttributeFields* ParaEngine::CParaWorldAsset::GetChildAttributeObject(const std::string& sName)
{
	for (IAttributeFields* pChild : m_attribute_models)
	{
		if (pChild->GetIdentifier() == sName || sName == pChild->GetAttributeClassName())
			return pChild;
	}
	return NULL;
}

IAttributeFields* ParaEngine::CParaWorldAsset::GetChildAttributeObject(int nRowIndex, int nColumnIndex /*= 0*/)
{
	if (nRowIndex < (int)m_attribute_models.size())
		return m_attribute_models[nRowIndex];
	return NULL;
}

int ParaEngine::CParaWorldAsset::GetChildAttributeObjectCount(int nColumnIndex /*= 0*/)
{
	return (int)m_attribute_models.size();
}

int ParaEngine::CParaWorldAsset::GetChildAttributeColumnCount()
{
	return 1;
}

bool ParaEngine::CParaWorldAsset::IsAsyncLoading() const
{
	return m_bAsyncLoading;
}

void ParaEngine::CParaWorldAsset::SetAsyncLoading(bool val)
{
	m_bAsyncLoading = val;
}

CEffectFile* CParaWorldAsset::LoadEffectFile(const string&  sIdentifier, const string&  fileName)
{
	string sFileName;
	CParaFile::ToCanonicalFilePath(sFileName, fileName, false);
	pair<CEffectFile*, bool> res = m_EffectsManager.CreateEntity(sIdentifier, sFileName);
	if (res.second == true)
	{
		CEffectFile* pNewEntity = res.first;
		pNewEntity->SetFileName(sFileName);
	}
	return res.first;
}

bool ParaEngine::CParaWorldAsset::IsAssetManifestEnabled() const
{
	return CAssetManifest::GetSingleton().IsEnabled();
}

void ParaEngine::CParaWorldAsset::EnableAssetManifest(bool val)
{
	CAssetManifest::GetSingleton().EnableManifest(val);
}

bool ParaEngine::CParaWorldAsset::IsUseLocalFileFirst() const
{
	return CAssetManifest::GetSingleton().IsUseLocalFileFirst();
}

void ParaEngine::CParaWorldAsset::SetUseLocalFileFirst(bool val)
{
	CAssetManifest::GetSingleton().SetUseLocalFileFirst(val);
}

#ifdef USE_DIRECTX_RENDERER

LatentOcclusionQueryBank* CParaWorldAsset::GetOcclusionQueryBank(int nID)
{
	LatentOcclusionQueryBank* pOcclusionQueryBank = NULL;
	if(nID>=0 && nID<(int)(m_pOcclusionQueryBanks.size()))
	{
		pOcclusionQueryBank = m_pOcclusionQueryBanks[nID];
	}

	if(pOcclusionQueryBank == NULL)
	{
		if(m_pOcclusionQuery != NULL )
		{
			if(nID>=0 && nID<100)
			{
				if(nID>=(int)m_pOcclusionQueryBanks.size())
					m_pOcclusionQueryBanks.resize(nID+1, 0);
				auto pRenderDevice = static_cast<CD3D9RenderDevice*>(CGlobals::GetRenderDevice());
				LPDIRECT3DDEVICE9 pd3dDevice = pRenderDevice->GetDirect3DDevice9();
				pOcclusionQueryBank = new LatentOcclusionQueryBank(pd3dDevice);
				if(!pOcclusionQueryBank->IsValid())
				{
					OUTPUT_LOG("warning: failed creating OcclusionQueryBank ID %d\n", nID);
					SAFE_DELETE(pOcclusionQueryBank);
				}
				m_pOcclusionQueryBanks[nID] = pOcclusionQueryBank;
			}
			else
			{
				OUTPUT_LOG("warning: can not GetOcclusionQueryBank ID %d is invalid \n", nID);
			}
		}
	}
	return pOcclusionQueryBank;
}


LPD3DXFILE CParaWorldAsset::GetParaXFileParser()
{
	return m_pDXFile;
}

void CParaWorldAsset::CreateXFileParser(LPD3DXFILE* ppParser)
{
	LPD3DXFILE pParser = NULL;
	try
	{
		// Create the .X file object
		if (FAILED(D3DXFileCreate(&pParser)))
			throw "error loading .X";

		/// Register the templates in use
		// register the standard retained mode templates from Direct3D
		if (FAILED(pParser->RegisterTemplates((LPVOID)D3DRM_XTEMPLATES, D3DRM_XTEMPLATE_BYTES)))
		{
			pParser->Release();
			throw "error register directx template";
		}
#ifdef PARAXTEMPLATE_XSKINEXP
		if (FAILED(pParser->RegisterTemplates((void*)XSKINEXP_TEMPLATES, strlen(XSKINEXP_TEMPLATES))))
		{
			pParser->Release();
			throw "error register directx template";
		}
#endif 		

		CParaFile file(":IDR_PARAXTEMPLATE");
		if (!file.isEof())
		{
			// register the ParaEngine X file templates 
			if (FAILED(pParser->RegisterTemplates((LPVOID)file.getBuffer(), file.getSize())))
			{
				pParser->Release();
				throw "error register paraengine x file template";
			}
		}
	}
	catch (char * err)
	{
		OUTPUT_LOG("%s\n", err);
	}
	if (ppParser)
	{
		*ppParser = pParser;
	}
}


int CParaWorldAsset::PrintToFile(CParaFile* pOutputFile, DWORD dwSelection/*=0xffffffff*/)
{
	int i = 0;
	if (dwSelection & 1)
	{
		i += GetTextureManager().PrintToFile(pOutputFile);
	}
	if (dwSelection & 2)
	{
		i += GetMeshManager().PrintToFile(pOutputFile);
	}
	if (dwSelection & 4)
	{
		i += GetParaXManager().PrintToFile(pOutputFile);
	}
	return i;
}


//--------------------------------------------------------------------
// Desc: Get Mesh Object by its identifier
// Params: 
//--------------------------------------------------------------------
D3DXSpriteEntity* CParaWorldAsset::GetD3DXSprite(const string& sIdentifier)
{
	return m_D3DXSpriteManager.GetByName(sIdentifier);
}

CadModel* CParaWorldAsset::LoadCadModel(const string& sIdentifier, const string& fileName)
{
	string sFileName;
	CParaFile::ToCanonicalFilePath(sFileName, fileName, false);
	if (m_bUseAssetSearch)
		DoAssetSearch(sFileName, CParaFile::GetCurDirectory(CParaFile::APP_MODEL_DIR).c_str());

	pair<CadModel*, bool> res = m_CadModelManager.CreateEntity(sIdentifier, sFileName);
	if (res.second == true)
	{
		CadModel* pNewEntity = res.first;
		pNewEntity->Init();
	}
	return res.first;
}

//--------------------------------------------------------------------
// Desc: Load new template into memory, template with the same identifier
//		will not be created twice. nFrames is the number of animation frames
//      are there in the sprite. It can be 1 for static image, or more than 1
//      for multiple images or even textured animation.
//		Basically, nRow =1, and nCol=nFrames
// Params: sIdentifier the identifier string
//--------------------------------------------------------------------
D3DXSpriteEntity* CParaWorldAsset::LoadD3DXSprite(const string&  sIdentifier, int nFrames, int nRow, int nCol)
{
	char keyStr[MAX_PATH];
	snprintf(keyStr, MAX_PATH, "%3d%3d%3d", nFrames, nRow, nCol);
	pair<D3DXSpriteEntity*, bool> res = m_D3DXSpriteManager.CreateEntity(sIdentifier, keyStr);
	if (res.second == true)
	{
		D3DXSpriteEntity* pNewEntity = res.first;
		pNewEntity->m_nRow = nRow;
		pNewEntity->m_nCol = nCol;
		pNewEntity->m_nFrames = nFrames;
	}
	return res.first;
}

#endif

int ParaEngine::CParaWorldAsset::InstallFields(CAttributeClass* pClass, bool bOverride)
{
	IAttributeFields::InstallFields(pClass, bOverride);
	pClass->AddField("AsyncLoading", FieldType_Bool, (void*)SetAsyncLoading_s, (void*)IsAsyncLoading_s, NULL, NULL, bOverride);
	pClass->AddField("EnableAssetManifest", FieldType_Bool, (void*)EnableAssetManifest_s, (void*)IsAssetManifestEnabled_s, NULL, NULL, bOverride);
	pClass->AddField("UseLocalFileFirst", FieldType_Bool, (void*)SetUseLocalFileFirst_s, (void*)IsUseLocalFileFirst_s, NULL, NULL, bOverride);
	pClass->AddField("DeleteTempDiskTextures", FieldType_void, (void*)DeleteTempDiskTextures_s, (void*)0, NULL, NULL, bOverride);
	return S_OK;
}
