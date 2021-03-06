/***************************************************************************
*                                                                          *
*  Copyright (C) Chaos Group & Golaem S.A. - All Rights Reserved.          *
*                                                                          *
***************************************************************************/

#include "vraygolaem.h"
#include "instance.h"
#include "pb2template_generator.h"
#include "defrayserver.h"
#include "vrender_unicode.h"
#include "hash_map.h"
#include "vraytexutils.h"
#include "pluginenumcallbacks.h"
#include "resource.h"
#include "maxscript/maxscript.h"

#include <fstream>	// std::ofstream
#include <sstream>	// std::stringstream
#include <io.h>		// _access

#if GET_MAX_RELEASE(VERSION_3DSMAX) >= 9000
#include "IPathConfigMgr.h"
#endif
#if GET_MAX_RELEASE(VERSION_3DSMAX) >= 11900
#include "IFileResolutionManager.h"
#pragma comment(lib, "assetmanagement.lib")
#endif

#define GLMC_IMPLEMENTATION
#define GLMC_NOT_INCLUDE_MINIZ
#include "glm_crowd.h"	// golaem cache reader
#include "glm_crowd_io.h"	// golaem cache reader

// V-Ray plugin ID for the 3ds Max material wrapper
#define MTL_WRAPPER_VRAY_ID LARGE_CONST(0x2015011056)

// no param block script access for VRay free
#ifdef _FREE_
#define _FT(X) _T("")
#define IS_PUBLIC 0
#else
#define _FT(X) _T(X)
#define IS_PUBLIC 1
#endif // _FREE_

//************************************************************
// Class descriptor
//************************************************************

class VRayGolaemClassDesc: public ClassDesc2 {
public:
	int IsPublic(void) { return IS_PUBLIC; }
	void *Create(BOOL loading) { return new VRayGolaem; }
	const TCHAR *ClassName(void) { return STR_CLASSNAME; }
	SClass_ID SuperClassID(void) { return GEOMOBJECT_CLASS_ID; }
	Class_ID ClassID(void) { return PLUGIN_CLASSID; }
	const TCHAR* Category(void) { return _T("VRay");  }

	// Hardwired name, used by MAX Script as unique identifier
	const TCHAR* InternalName(void) { return STR_INTERNALNAME; }
	HINSTANCE HInstance(void) { return hInstance; }
};

//************************************************************
// Static / Define variables
//************************************************************

#define BIGFLOAT	float(999999) // from bendmod sample
#define BIGINT		int(999999)
#define ICON_RADIUS 2
#define CROWDVRAYPLUGINID PluginID(LARGE_CONST(2011070866)) // from glmCrowdVRayPlugin.h

TCHAR *iconText=_T("VRayGolaem");
static VRayGolaemClassDesc vrayGolaemClassDesc;

// The names of the node user properties that V-Ray uses for reflection/refraction visibility.
// V-Ray doesn't publish the header with their definitions so I copy them here.
#define PROP_GI_VISIBLETOREFL _T("VRay_GI_VisibleToReflections")
#define PROP_GI_VISIBLETOREFR _T("VRay_GI_VisibleToRefractions")
#define PROP_MOBLUR_USEDEFAULTGEOMSAMPLES _T("VRay_MoBlur_DefaultGeomSamples")
#define PROP_MOBLUR_GEOMSAMPLES _T("VRay_MoBlur_GeomSamples")
#define PROP_MOBLUR_OVERRIDEDURATION _T("VRay_MoBlur_Override")
#define PROP_MOBLUR_DURATION _T("VRay_MoBlur_Override_Duration")


//************************************************************
// DLL stuff
//************************************************************

HINSTANCE hInstance;
int controlsInit=FALSE;

BOOL WINAPI DllMain(HINSTANCE hinstDLL,ULONG fdwReason,LPVOID lpvReserved) {
	hInstance=hinstDLL;

	if (!controlsInit) {
		controlsInit=TRUE;
#if MAX_RELEASE<13900
		InitCustomControls(hInstance);
#endif
		InitCommonControls();
	}

	return(TRUE);
}

__declspec( dllexport ) const TCHAR* LibDescription(void) { return STR_LIBDESC; }
__declspec( dllexport ) int LibNumberClasses(void) { return 1; }

__declspec( dllexport ) ClassDesc* LibClassDesc(int i) {
	switch(i) { case 0: return &vrayGolaemClassDesc; }
	return NULL;
}

__declspec( dllexport ) ULONG LibVersion(void) { return VERSION_3DSMAX; }

__declspec( dllexport ) int LibInitialize(void) { return TRUE; }

__declspec( dllexport ) int LibShutdown(void) {
	if (golaemPlugman) {
		golaemPlugman->deleteAll();
		golaemPlugman->unloadAll();
		deleteDefaultPluginManager(golaemPlugman);
		golaemPlugman=NULL;
	}
	return TRUE;
}

class VRayGolaemDlgProc: public ParamMap2UserDlgProc {
	void chooseFileName(IParamBlock2 *pblock2, ParamID paramID, const TCHAR *title);
public:
	VRayGolaemDlgProc() {}

	INT_PTR DlgProc(TimeValue t, IParamMap2 *map, HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	void DeleteThis() { }

	void SetThing(ReferenceTarget *m) {}
};

static VRayGolaemDlgProc vrayGolaemDlgProc;

// Find the node that references this Golaem object. If the Golaem object is instanced,
// it is not defined which node is returned.
INode *getNode(VRayGolaem *golaem) {
	ULONG handle=0;
	golaem->NotifyDependents(FOREVER, (PartID)&handle, REFMSG_GET_NODE_HANDLE);
	INode *node=GetCOREInterface()->GetINodeByHandle(handle);
	return node;
}

//------------------------------------------------------------
bool fileExists(const CStr& pathname)
{
	return ( _waccess( pathname.ToWStr(), 0 ) == 0 ) ;
}

//************************************************************
// Parameter block
//************************************************************

// Paramblock2 name
enum { params, }; 

static ParamBlockDesc2 param_blk(params, STR_DLGTITLE,  0, &vrayGolaemClassDesc, P_AUTO_CONSTRUCT+P_AUTO_UI, REFNO_PBLOCK,
	IDD_VRAYGOLAEM, IDS_VRAYGOLAEM_PARAMS, 0, 0, &vrayGolaemDlgProc,
	// Params
	pb_file, _T("cache_file"), TYPE_FILENAME, P_RESET_DEFAULT, 0,
		p_ui, TYPE_EDITBOX, ED_GOLAEMVRSCENE,
#if GET_MAX_RELEASE(VERSION_3DSMAX) >= 11900
		p_assetTypeID, MaxSDK::AssetManagement::AssetType::kExternalLink,
#endif
	PB_END,
	pb_shaders_file, _T("shaders_file"), TYPE_FILENAME, P_RESET_DEFAULT, 0,
		p_ui, TYPE_EDITBOX, ED_SHADERSVRSCENE,
#if GET_MAX_RELEASE(VERSION_3DSMAX) >= 11900
		p_assetTypeID, MaxSDK::AssetManagement::AssetType::kExternalLink,
#endif
	PB_END,

	// display attributes
	pb_enable_display, _T("enable_display"), TYPE_BOOL, P_RESET_DEFAULT, 0,
	p_default, TRUE,
	p_ui, TYPE_SINGLECHEKBOX, ED_DISPLAYENABLE,
	PB_END,
	pb_display_percentage, _T("display_percentage"), TYPE_FLOAT, P_RESET_DEFAULT, 0,
	p_default, 100.f,
	p_range, 0.f, 100.f, 
	p_ui, TYPE_SPINNER,  EDITTYPE_FLOAT, ED_DISPLAYPERCENT, ED_DISPLAYPERCENTSPIN, 1.f,
	PB_END,
	pb_display_entity_ids, _T("display_entity_ids"), TYPE_BOOL, P_RESET_DEFAULT, 0,
	p_default, TRUE,
	p_ui, TYPE_SINGLECHEKBOX, ED_DISPLAYENTITYIDS,
	PB_END,

	// cache attributes
	pb_crowd_fields, _T("crowd_fields"), TYPE_STRING, P_RESET_DEFAULT, 0,
	p_ui, TYPE_EDITBOX, ED_CROWDFIELDS,
	PB_END,
	pb_cache_name, _T("cache_name"), TYPE_STRING, P_RESET_DEFAULT, 0,
	p_ui, TYPE_EDITBOX, ED_CACHENAME,
	PB_END,
	pb_cache_dir, _T("cache_dir"), TYPE_STRING, P_RESET_DEFAULT, 0,
	p_ui, TYPE_EDITBOX, ED_CACHEDIR,
	PB_END,
	pb_character_files, _T("character_files"), TYPE_STRING, P_RESET_DEFAULT, 0,
	p_ui, TYPE_EDITBOX, ED_CHARACTERFILES,
	PB_END,
	// layout attributes
	pb_layout_enable, _T("layout_enable"), TYPE_BOOL, P_RESET_DEFAULT, 0,
	p_default, TRUE,
	p_ui, TYPE_SINGLECHEKBOX, ED_LAYOUTENABLE,
	PB_END,
	pb_layout_name, _T("layout_file"), TYPE_STRING, P_RESET_DEFAULT, 0,
	p_ui, TYPE_EDITBOX, ED_LAYOUTNAME,
	PB_END,
	pb_layout_dir, _T("layout_directory"), TYPE_STRING, P_RESET_DEFAULT, 0,
	p_ui, TYPE_EDITBOX, ED_LAYOUTDIR,
	PB_END,
	pb_terrain_file, _T("terrain_file"), TYPE_STRING, P_RESET_DEFAULT, 0,
	p_ui, TYPE_EDITBOX, ED_TERRAINFILE,
	PB_END,
	// culling attributes
	pb_frustum_enable, _T("frustum_enable"), TYPE_BOOL, P_RESET_DEFAULT, 0,
	p_default, FALSE,
	p_ui, TYPE_SINGLECHEKBOX, ED_FRUSTUMENABLE,
	PB_END,
	pb_frustum_margin, _T("frustum_margin"), TYPE_FLOAT, P_RESET_DEFAULT, 0,
	p_default, 10.f,
	p_range, -BIGFLOAT, BIGFLOAT, 
	p_ui, TYPE_SPINNER, EDITTYPE_FLOAT, ED_FRUSTUMMARGIN, ED_FRUSTUMMARGINSPIN, 1.f,
	PB_END,
	pb_camera_margin, _T("camera_margin"), TYPE_FLOAT, P_RESET_DEFAULT, 0,
	p_default, 10.f,
	p_range, -BIGFLOAT, BIGFLOAT, 
	p_ui, TYPE_SPINNER, EDITTYPE_FLOAT, ED_CAMERAMARGIN, ED_CAMERAMARGINSPIN, 1.f,
	PB_END,

	// vray attributes
	pb_frame_offset, _T("frame_offset"), TYPE_INT, P_RESET_DEFAULT, 0,
	p_default, 0,
	p_range, -BIGINT, BIGINT, 
	p_ui, TYPE_SPINNER,  EDITTYPE_INT, ED_FRAMEOFFSET, ED_FRAMEOFFSETSPIN, 1,
	PB_END,
	pb_object_id_mode, _T("objectId_mode"), TYPE_INT, P_RESET_DEFAULT, 0,
    p_ui, TYPE_INT_COMBOBOX, CB_OBJECTIDMODE, 4, CB_OBJECTIDMODE_ITEM1, CB_OBJECTIDMODE_ITEM2, CB_OBJECTIDMODE_ITEM3, CB_OBJECTIDMODE_ITEM4, 
	p_vals, 0, 1, 2, 3,
	p_default, 0,
	PB_END,	
	pb_default_material, _T("default_material"), TYPE_STRING, P_RESET_DEFAULT, 0,
	p_ui, TYPE_EDITBOX, ED_DEFAULTMATERIAL,
	PB_END,
	pb_temp_vrscene_file_dir, _T("temp_vrscene_file_dir"), TYPE_STRING, P_RESET_DEFAULT, 0,
	p_default, _T("TEMP"),
	p_ui, TYPE_EDITBOX, ED_TEMPVRSCENEFILEDIR,
	PB_END,
	pb_instancing_enable, _T("instancing_enable"), TYPE_BOOL, P_RESET_DEFAULT, 0,
	p_default, TRUE,
	p_ui, TYPE_SINGLECHEKBOX, ED_INSTANCINGENABLE,
	PB_END,

	// not used anymore but kept for retrocomp
	pb_display_percent, _T(""), TYPE_INT, 0, 0, PB_END,
	pb_use_node_attributes, _T(""), TYPE_BOOL, 0, 0, PB_END,
	pb_motion_blur_enable, _T(""), TYPE_BOOL, 0, 0, PB_END,
	pb_motion_blur_start, _T(""), TYPE_FLOAT, 0, 0, PB_END,
	pb_motion_blur_window_size, _T(""), TYPE_FLOAT, 0, 0, PB_END,
	pb_motion_blur_samples, _T(""), TYPE_INT, 0, 0, PB_END,
	pb_scale_transform, _T(""), TYPE_FLOAT, 0, 0, PB_END,
	pb_object_id_base, _T(""), TYPE_INT, 0, 0, PB_END,
	pb_primary_visibility, _T(""), TYPE_BOOL, 0, 0, PB_END,
	pb_casts_shadows, _T(""), TYPE_BOOL, 0, 0, PB_END,
	pb_visible_in_reflections, _T(""), TYPE_BOOL, 0, 0, PB_END,
	pb_visible_in_refractions, _T(""), TYPE_BOOL, 0, 0, PB_END,
	pb_override_node_properties, _T(""), TYPE_BOOL, 0, 0, PB_END,
	pb_excluded_entities, _T(""), TYPE_STRING, 0, 0, PB_END,

PB_END
);

//************************************************************
// VRayGolaem implementation
//************************************************************

//------------------------------------------------------------
// VRayGolaem
//------------------------------------------------------------
VRayGolaem::VRayGolaem() 
	: _simulationData(NULL), _frameData(NULL), _updateCacheData(true)
{
	static int pblockDesc_inited=false;
	if (!pblockDesc_inited) 
	{
		initPBlockDesc(param_blk);
		pblockDesc_inited=true;
	}
	pblock2=NULL;
	vrayGolaemClassDesc.MakeAutoParamBlocks(this);
	assert(pblock2);
	suspendSnap=FALSE;
}

VRayGolaem::~VRayGolaem() {
}

//------------------------------------------------------------
// Misc
//------------------------------------------------------------
void VRayGolaem::InvalidateUI() {
	param_blk.InvalidateUI(pblock2->LastNotifyParamID());
}

static Pb2TemplateGenerator templateGenerator;

void VRayGolaem::BeginEditParams(IObjParam *ip, ULONG flags, Animatable *prev) {
	vrayGolaemClassDesc.BeginEditParams(ip, this, flags, prev);
}

void VRayGolaem::EndEditParams(IObjParam *ip, ULONG flags, Animatable *next) {
	vrayGolaemClassDesc.EndEditParams(ip, this, flags, next);
}

RefTargetHandle VRayGolaem::Clone(RemapDir& remap) {
	VRayGolaem* newob=new VRayGolaem();	
	BaseClone(this, newob, remap);
	newob->ReplaceReference(0, pblock2->Clone(remap));
	return newob;
}

Animatable* VRayGolaem::SubAnim(int i) {
	switch (i) {
		case 0: return pblock2;
		default: return NULL;
	}
}

TSTR VRayGolaem::SubAnimName(int i) {
	switch (i) {
		case 0: return STR_DLGTITLE;
		default: return _T("");
	}
}

RefTargetHandle VRayGolaem::GetReference(int i) {
	switch (i) {
		case REFNO_PBLOCK: return pblock2;
		default: return NULL;
	}
}

void VRayGolaem::SetReference(int i, RefTargetHandle rtarg) {
	switch (i) {
		case REFNO_PBLOCK: pblock2 = (IParamBlock2*)rtarg; break;
	}
}

RefResult VRayGolaem::NotifyRefChanged(NOTIFY_REF_CHANGED_ARGS) {
	switch (message) {
		case REFMSG_CHANGE:
			if (hTarget==pblock2) {
				ParamID paramID=pblock2->LastNotifyParamID();
				switch (paramID) {
					case pb_frustum_enable:
						grayDlgControls();
						break;
				}
				param_blk.InvalidateUI();
			}
			break;
	}
	return REF_SUCCEED;
}

Interval VRayGolaem::ObjectValidity(TimeValue t) {
	return Interval(t,t);
}

//------------------------------------------------------------
// proc
//------------------------------------------------------------
int VRayGolaemCreateCallBack::proc(ViewExp *vpt, int msg, int point, int flags, IPoint2 m, Matrix3& mat) {
	if (!sphere) return CREATE_ABORT;

	Point3 np=vpt->SnapPoint(m,m,NULL,SNAP_IN_PLANE);

	switch (msg) {
		case MOUSE_POINT:
			switch (point) {
				case 0:
					sphere->suspendSnap=TRUE;				
					sp0=m;
					p0=vpt->SnapPoint(m, m, NULL, SNAP_IN_3D);
					mat.SetTrans(p0);
				case 1:
					return CREATE_STOP;
			}
			return CREATE_CONTINUE;

		case MOUSE_MOVE:
			return CREATE_CONTINUE;

		case MOUSE_ABORT:
			return CREATE_ABORT;
	}

	return CREATE_CONTINUE;
}

static VRayGolaemCreateCallBack createCB;

CreateMouseCallBack* VRayGolaem::GetCreateMouseCallBack() {
	createCB.SetObj(this);
	return &createCB;
}

void VRayGolaem::SetExtendedDisplay(int flags) {
}

void VRayGolaem::GetLocalBoundBox(TimeValue t, INode* inode, ViewExp* vpt, Box3& box) 
{
	float radius=ICON_RADIUS; 
	_nodeBbox+=Point3(-radius, -radius, -radius);
	_nodeBbox+=Point3(radius, radius, radius);
	box = _nodeBbox;
}

void VRayGolaem::GetWorldBoundBox(TimeValue t, INode* inode, ViewExp* vpt, Box3& box) {
	if (!inode) return;
	Box3 localBox;
	GetLocalBoundBox(t, inode, vpt, localBox);
	box=localBox*(inode->GetObjectTM(t));
}

void VRayGolaem::GetDeformBBox(TimeValue t, Box3 &b, Matrix3 *tm, BOOL useSel) {
	if (!tm) GetLocalBoundBox(t, NULL, NULL, b);
	else {
		Box3 bbox;
		GetLocalBoundBox(t, NULL, NULL, bbox);
		b.Init();
		for (int i=0; i<8; i++) b+=(*tm)*bbox[i];
	}
}

int VRayGolaem::HitTest(TimeValue t, INode *node, int type, int crossing, int flags, IPoint2 *p, ViewExp *vpt) {
	static HitRegion hitRegion;
	DWORD	savedLimits;

	GraphicsWindow *gw=vpt->getGW();	
	Material *mtl=gw->getMaterial();
	MakeHitRegion(hitRegion, type, crossing, 4, p);

	gw->setRndLimits(((savedLimits = gw->getRndLimits())|GW_PICK)&~GW_ILLUM);
	gw->setHitRegion(&hitRegion);
	gw->clearHitCode();

	draw(t, node, vpt);

	gw->setRndLimits(savedLimits);
	
	if((hitRegion.type != POINT_RGN) && !hitRegion.crossing) return TRUE;
	return gw->checkHitCode();
}

void VRayGolaem::Snap(TimeValue t, INode* inode, SnapInfo *snap, IPoint2 *p, ViewExp *vpt) {
	if (suspendSnap) return;
}

//------------------------------------------------------------
// Display
//------------------------------------------------------------
int VRayGolaem::Display(TimeValue t, INode* node, ViewExp *vpt, int flags) {
	draw(t, node, vpt);
	return 0;
}

ObjectState VRayGolaem::Eval(TimeValue time) 
{
	_updateCacheData = true; // time has changed, we should re-read the cache
	return ObjectState(this);
}

void* VRayGolaem::GetInterface(ULONG id) {
	if (id==I_VRAYGEOMETRY) return (VR::VRenderObject*) this;
	return GeomObject::GetInterface(id);
}

void VRayGolaem::ReleaseInterface(ULONG id, void *ip) {
	if (id==I_VRAYGEOMETRY) return;
	GeomObject::ReleaseInterface(id, ip);
}

Mesh* VRayGolaem::GetRenderMesh(TimeValue t, INode *inode, View& view, BOOL& needDelete) {
	needDelete=false;
	return &_mesh;
}

INT_PTR VRayGolaemDlgProc::DlgProc(TimeValue t, IParamMap2 *map, HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	int id=LOWORD(wParam);

	IParamBlock2 *pblock=NULL;
	VRayGolaem *vrayGolaem=NULL;

	if (map) pblock=map->GetParamBlock();
	if (pblock) vrayGolaem= static_cast<VRayGolaem*>(pblock->GetOwner());

	switch (msg) {
		case WM_INITDIALOG: {
			if (vrayGolaem) vrayGolaem->grayDlgControls();
			break;
		}
		case WM_DESTROY:
			break;
		case WM_COMMAND: {
			int ctrlID=LOWORD(wParam);
			int notifyCode=HIWORD(wParam);
			HWND ctrlHWnd=(HWND) lParam;

			if (notifyCode==BN_CLICKED) {
				if (ctrlID==BN_GOLAEMBROWSE && vrayGolaem) {
					chooseFileName(pblock, pb_file, _T("Choose Golaem .vrscene file"));
					// if the vrscene has been loaded, fill the node attributes
					const TCHAR *fname_wstr=pblock->GetStr(pb_file, t);
					if (fname_wstr)
					{
						GET_MBCS(fname_wstr, fname_mbcs);
						vrayGolaem->readCrowdVRScene(fname_mbcs);
					}
				}
				if (ctrlID==BN_SHADERSBROWSE && vrayGolaem) {
					chooseFileName(pblock, pb_shaders_file, _T("Choose shaders .vrscene file"));
				}
				if (ctrlID==BN_MATERIALSCREATE && vrayGolaem) {
					// call post creation python script
					INode* node=getNode(vrayGolaem);
					if (node == NULL)
					{
						CStr logMessage = CStr("VRayGolaem: This object is an 3ds Max instance and is not supported. Please create a copy.");
						mprintf(logMessage.ToBSTR());
						return FALSE;
					}

					GET_MBCS(node->GetName(), nodeName);
					CStr sourceCmd = CStr("python.ExecuteFile \"vraygolaem.py\"");
					ExecuteMAXScriptScript(sourceCmd.ToBSTR());
					CStr callbackCmd = CStr("python.Execute \"glmVRayGolaemPostCreationCallback('") + CStr(nodeName) + CStr("')\"");
					ExecuteMAXScriptScript(callbackCmd.ToBSTR());
				}
			}
			break;
		}
	}

	return FALSE;
}

void VRayGolaem::grayDlgControls(void) {
	IParamMap2 *map=pblock2->GetMap();
	if (!map)
		return; // If no UI - nothing to do

	HWND hWnd=map->GetHWnd();

	// Frustum culling
	int fcull=pblock2->GetInt(pb_frustum_enable);
	map->Enable(pb_frustum_margin, fcull);
	map->Enable(pb_camera_margin, fcull);

	EnableWindow(GetDlgItem(hWnd, ST_CULLFRUSTUM), fcull);
	EnableWindow(GetDlgItem(hWnd, ST_CULLCAMERA), fcull);
}


//************************************************************
// Browse
//************************************************************

static const TCHAR *vrsceneExtList=_T("V-Ray scene file (*.vrscene)\0*.vrscene\0All files(*.*)\0*.*\0\0");
static const TCHAR *vrsceneDefExt=_T("vrscene");

//------------------------------------------------------------
// chooseFileName
//------------------------------------------------------------
void VRayGolaemDlgProc::chooseFileName(IParamBlock2 *pblock2, ParamID paramID, const TCHAR *title) {
	TCHAR fname[512]=_T("");
	fname[0]='\0';

	const TCHAR *storedName=pblock2->GetStr(paramID);
	if (storedName) vutils_strcpy_n(fname, storedName, COUNT_OF(fname));

	OPENFILENAME fn;
	fn.lStructSize=sizeof(fn);
	fn.hwndOwner=GetCOREInterface()->GetMAXHWnd();
	fn.hInstance=hInstance;
	fn.lpstrFilter=vrsceneExtList;
	fn.lpstrCustomFilter=NULL;
	fn.nMaxCustFilter=0;
	fn.nFilterIndex=1;
	fn.lpstrFile=fname;
	fn.nMaxFile=512;
	fn.lpstrFileTitle=NULL;
	fn.nMaxFileTitle=0;
	fn.lpstrInitialDir=NULL;
	fn.lpstrTitle=title;
	fn.Flags=0;
	fn.lpstrDefExt=vrsceneDefExt;
	fn.lCustData=NULL;
	fn.lpfnHook=NULL;
	fn.lpTemplateName=NULL;

	BOOL res=GetOpenFileName(&fn);

	const TCHAR *fullFname=NULL;
#if GET_MAX_RELEASE(VERSION_3DSMAX) >= 9000
	MaxSDK::Util::Path fpath(fname);
#if GET_MAX_RELEASE(VERSION_3DSMAX) < 11900
	IPathConfigMgr::GetPathConfigMgr()->NormalizePathAccordingToSettings(fpath);
	fullFname=fpath.GetCStr();
#else
	TSTR mstrfname(fname);
	IFileResolutionManager::GetInstance()->DoGetUniversalFileName(mstrfname);
	fullFname=mstrfname.data();
#endif
#else
	fullFname = fname;
#endif

	if (res) {
		pblock2->SetValue(paramID, 0, const_cast<TCHAR*>(fullFname));
		IParamMap2 *map=pblock2->GetMap();
		if (map) map->Invalidate(paramID);
	}
}

//************************************************************
// Draw
//************************************************************

//------------------------------------------------------------
// readGolaemCache
//------------------------------------------------------------
void VRayGolaem::readGolaemCache(TimeValue t)
{
	if (!_updateCacheData) return;
	
	// clean previous data
	for (size_t iData=0, nbData=_simulationData.length(); iData<nbData; ++iData)
	{
		glmDestroyFrameData(&_frameData[iData], _simulationData[iData]);
		glmDestroySimulationData(&_simulationData[iData]);
	}
	_simulationData.removeAll();
	_frameData.removeAll();

	// update params
	updateVRayParams(t);
	_updateCacheData = false;

	// read caches
	MaxSDK::Array<CStr> crowdFields;
	splitStr(_crowdFields, ';', crowdFields);
	if (_cacheName.length() != 0 && _cacheDir.length() != 0)
	{
		for (size_t iCf=0, nbCf=crowdFields.length(); iCf<nbCf; ++iCf)
		{
			int currentFrame = (int)((float)t / (float)TIME_TICKSPERSEC * (float)GetFrameRate()) + _frameOffset; 
			CStr currentFrameStr; currentFrameStr.printf("%i", currentFrame);
			CStr cachePrefix(_cacheDir + "/" + _cacheName + "." + crowdFields[iCf] + ".");
			CStr cacheStream(cachePrefix + "%d.gscf");
			CStr gscsFileStr(cachePrefix + "gscs");
			CStr gscfFileStr(cachePrefix + currentFrameStr + ".gscf");
			CStr gsclFileStr(_layoutDir + "/" + _layoutName + "." + crowdFields[iCf] + ".gscl");
			CStr srcTerrainFile(cachePrefix + "terrain.fbx");

			// load gscs
			GlmSimulationCacheStatus status;
			GlmSimulationData* simulationData(NULL);
			GlmFrameData* frameData(NULL);
			status = glmCreateAndReadSimulationData(&simulationData, gscsFileStr);
			if (status != GSC_SUCCESS)
			{
				glmDestroySimulationData(&simulationData);
				DebugPrint(_T("VRayGolaem: Error loading .gscs file \"%s\""), gscsFileStr);
				return;
			}

			// load gscf
			glmCreateFrameData(&frameData, simulationData);
			status = glmReadFrameData(frameData, simulationData, gscfFileStr);
			if (status != GSC_SUCCESS)
			{
				glmDestroyFrameData(&frameData, simulationData);
				DebugPrint(_T("VRayGolaem: Error loading .gscf file \"%s\""), gscfFileStr);
				return;
			}

			// load gscl
			GlmHistory* history = NULL;
			GlmEntityTransform* entityTransforms = NULL;
			int entityTransformCount(0);

			if (_layoutEnable)
			{
				GlmSimulationCacheStatus gsclStatus = glmCreateAndReadHistoryJSON(&history, gsclFileStr);
				if (gsclStatus == GSC_SUCCESS)
				{
					// Terrain
					/*
					CrowdTerrain::Mesh* terrainMeshSource(NULL), *terrainMeshDestination(NULL);
					if (srcTerrainFile.Length()) terrainMeshSource = CrowdTerrain::loadTerrainAsset(srcTerrainFile);
					if (_terrainFile.Length()) terrainMeshDestination = CrowdTerrain::loadTerrainAsset(_terrainFile);
					if (terrainMeshDestination == NULL) terrainMeshDestination = terrainMeshSource;
					
					history->_terrainMeshSource = terrainMeshSource;
					history->_terrainMeshDestination = terrainMeshDestination;
					*/

					glmCreateEntityTransforms(simulationData, history, &entityTransforms, &entityTransformCount);

					GlmSimulationData* simulationDataOut;
					GlmFrameData* frameDataOut;
					glmCreateModifiedSimulationData(simulationData, entityTransforms, entityTransformCount, &simulationDataOut);
					glmCreateModifiedFrameData(simulationData, frameData, entityTransforms, entityTransformCount, history, simulationDataOut, &frameDataOut, currentFrame, cacheStream, _cacheDir);

					// replace previous simulation & frame data
					glmDestroyFrameData(&frameData, simulationData);
					glmDestroySimulationData(&simulationData);
					frameData = frameDataOut;
					simulationData = simulationDataOut;

					// Delete Terrain
					/*
					if (terrainMeshSource && terrainMeshSource != terrainMeshDestination) CrowdTerrain::closeTerrainAsset(terrainMeshSource);
					if (terrainMeshDestination) CrowdTerrain::closeTerrainAsset(terrainMeshDestination);
					*/
				}
			}

			if (history) glmDestroyHistory(&history);
			if (entityTransforms) glmDestroyEntityTransforms(&entityTransforms, entityTransformCount);
			
			_simulationData.append(simulationData);
			_frameData.append(frameData);
			
		}
	}
}

//------------------------------------------------------------
// drawEntities
//------------------------------------------------------------
void VRayGolaem::drawEntities(GraphicsWindow *gw, const Matrix3& transform, TimeValue t)
{
	// get display attributes
	bool displayEnable = pblock2->GetInt(pb_enable_display, t) == 1;
	float displayPercent = pblock2->GetFloat(pb_display_percentage, t);
	bool displayEntityIds = pblock2->GetInt(pb_display_entity_ids, t) == 1;
	if (!displayEnable) return;

	// update cache if required
	readGolaemCache(t);
	if (_simulationData.length() == 0 || _frameData.length() == 0 || _simulationData.length() != _frameData.length()) return;

	// draw
	_nodeBbox.Init();
	
	float transformScale(transform.GetRow(0).Length());
	for (size_t iData=0, nbData=_simulationData.length(); iData<nbData; ++iData)
	{
		int maxDisplayedEntity = (int)(_simulationData[iData]->_entityCount * displayPercent / 100.f);
		for (size_t iEntity=0, entityCount = maxDisplayedEntity; iEntity<entityCount; ++iEntity)
		{
			int64_t entityId = _simulationData[iData]->_entityIds[iEntity];
			if (entityId == -1) continue;

			unsigned int entityType = _simulationData[iData]->_entityTypes[iEntity];
			float entityRadius = _simulationData[iData]->_entityRadius[iEntity] * transformScale;
			float entityHeight = _simulationData[iData]->_entityHeight[iEntity] * transformScale;
			if(_simulationData[iData]->_boneCount[entityType])
			{
				// draw bbox
				unsigned int iBoneIndex = _simulationData[iData]->_iBoneOffsetPerEntityType[entityType] + _simulationData[iData]->_indexInEntityType[iEntity] * _simulationData[iData]->_boneCount[entityType];
				Point3 entityPosition(_frameData[iData]->_bonePositions[iBoneIndex][0], _frameData[iData]->_bonePositions[iBoneIndex][1], _frameData[iData]->_bonePositions[iBoneIndex][2]);
				// axis transformation for max
				entityPosition = entityPosition * transform;
				Box3 entityBbox(Point3(entityPosition[0]-entityRadius, entityPosition[1]-entityRadius, entityPosition[2]), Point3(entityPosition[0]+entityRadius, entityPosition[1]+entityRadius, entityPosition[2]+entityHeight));
				drawBBox(gw, entityBbox); // update node bbox
				_nodeBbox += entityBbox;

				// draw EntityID
				if (displayEntityIds)
				{
					CStr entityIdStrs; entityIdStrs.printf("%i", entityId);
					drawText(gw, entityIdStrs.ToMCHAR(), entityPosition);
				}
			}
		}
	}
}

//------------------------------------------------------------
// draw
//------------------------------------------------------------
void VRayGolaem::draw(TimeValue t, INode *node, ViewExp *vpt) 
{
	GraphicsWindow *gw=vpt->getGW();
	Matrix3 tm=node->GetObjectTM(t);
	gw->setTransform(tm);

	Color color=Color(node->GetWireColor());
	if (node->IsFrozen()) color=GetUIColor(COLOR_FREEZE);
	else if (node->Selected()) color=GetUIColor(COLOR_SELECTION);
	gw->setColor(LINE_COLOR, color);

	// locator
	drawSphere(gw, Point3::Origin, ICON_RADIUS, 30);

	// entities	
	drawEntities(gw, tm, t);

	// text
	tm.NoScale();
	float scaleFactor=vpt->NonScalingObjectSize()*vpt->GetVPWorldWidth(tm.GetTrans())/(float)360.0;
	tm.Scale(Point3(scaleFactor,scaleFactor,scaleFactor));
	gw->setTransform(tm);
	drawText(gw, iconText, Point3::Origin);
}

//************************************************************
// VRenderObject
//************************************************************

#if MAX_RELEASE >= 6000 && MAX_RELEASE < 8900
	#define VRAYRT_MAIN    "VRAY30_RT_FOR_3DSMAX60_MAIN"
	#define VRAYRT_PLUGINS "VRAY30_RT_FOR_3DSMAX60_PLUGINS"
#elif MAX_RELEASE >= 8900 && MAX_RELEASE < 10900
	#define VRAYRT_MAIN    "VRAY30_RT_FOR_3DSMAX90_MAIN"
	#define VRAYRT_PLUGINS "VRAY30_RT_FOR_3DSMAX90_PLUGINS"
#elif MAX_RELEASE >= 10900 && MAX_RELEASE < 11900
	#define VRAYRT_MAIN    "VRAY30_RT_FOR_3DSMAX2009_MAIN"
	#define VRAYRT_PLUGINS "VRAY30_RT_FOR_3DSMAX2009_PLUGINS"
#elif MAX_RELEASE >= 11900 && MAX_RELEASE < 12900
	#define VRAYRT_MAIN    "VRAY30_RT_FOR_3DSMAX2010_MAIN"
	#define VRAYRT_PLUGINS "VRAY30_RT_FOR_3DSMAX2010_PLUGINS"
#elif MAX_RELEASE >= 12900 && MAX_RELEASE < 13900
	#define VRAYRT_MAIN    "VRAY30_RT_FOR_3DSMAX2011_MAIN"
	#define VRAYRT_PLUGINS "VRAY30_RT_FOR_3DSMAX2011_PLUGINS"
#elif MAX_RELEASE >= 13900 && MAX_RELEASE < 14900
	#define VRAYRT_MAIN    "VRAY30_RT_FOR_3DSMAX2012_MAIN"
	#define VRAYRT_PLUGINS "VRAY30_RT_FOR_3DSMAX2012_PLUGINS"
#elif MAX_RELEASE >= 14850 && MAX_RELEASE < 15900
	#define VRAYRT_MAIN    "VRAY30_RT_FOR_3DSMAX2013_MAIN"
	#define VRAYRT_PLUGINS "VRAY30_RT_FOR_3DSMAX2013_PLUGINS"
#elif MAX_RELEASE >= 15850 && MAX_RELEASE < 16900
	#define VRAYRT_MAIN    "VRAY30_RT_FOR_3DSMAX2014_MAIN"
	#define VRAYRT_PLUGINS "VRAY30_RT_FOR_3DSMAX2014_PLUGINS"
#elif MAX_RELEASE >= 16850 && MAX_RELEASE < 17900
	#define VRAYRT_MAIN    "VRAY30_RT_FOR_3DSMAX2015_MAIN"
	#define VRAYRT_PLUGINS "VRAY30_RT_FOR_3DSMAX2015_PLUGINS"
#elif MAX_RELEASE >= 17850 && MAX_RELEASE < 18900
	#define VRAYRT_MAIN    "VRAY30_RT_FOR_3DSMAX2016_MAIN"
	#define VRAYRT_PLUGINS "VRAY30_RT_FOR_3DSMAX2016_PLUGINS"
#elif MAX_RELEASE >= 18850 && MAX_RELEASE < 19900
	#define VRAYRT_MAIN    "VRAY30_RT_FOR_3DSMAX2017_MAIN"
	#define VRAYRT_PLUGINS "VRAY30_RT_FOR_3DSMAX2017_PLUGINS"
#else
#error Unsupported version of 3ds Max API
#endif

//------------------------------------------------------------
// init
//------------------------------------------------------------
int VRayGolaem::init(const ObjectState &os, INode *node, VR::VRayCore *vray) 
{
	VRenderObject::init(os, node, vray);
	return true;
}

//------------------------------------------------------------
// Get the path to the V-Ray plugins; use the V-Ray RT environment variable for this
//------------------------------------------------------------
const tchar* getVRayPluginPath() 
{
	char pluginsDirVar[512];
	vutils_strcpy(pluginsDirVar, VRAYRT_PLUGINS);
	vutils_strcat(pluginsDirVar, "_");
	vutils_strcat(pluginsDirVar, PROCESSOR_ARCHITECTURE);

	const char *s = getenv(pluginsDirVar);
	if (s == NULL) {
		tchar str[512];
		sprintf(str, "Could not read V-Ray environment variable \"%s\"\n", pluginsDirVar);
		VUtils::debug(str);
		return ".";
	}
	return s;
}

CStr getStrParam(IParamBlock2* block, ParamID id, TimeValue t, const CStr& defaultStr="")
{
	CStr returnedString=defaultStr;
	const TCHAR *param_wstr=block->GetStr(id, t);
	if (param_wstr)  
	{
		GET_MBCS(param_wstr, param_mbcs);
		returnedString=param_mbcs;
	}
	return returnedString;
}

//------------------------------------------------------------
// updateVRayParams
//------------------------------------------------------------
void VRayGolaem::updateVRayParams(TimeValue t) 
{
	// check if this object is not an instance (then it has no max node to query)
	INode* node=getNode(this);
	if (node == NULL)
	{
		CStr logMessage = CStr("VRayGolaem: This object is an 3ds Max instance and is not supported. Please create a copy.");
		mprintf(logMessage.ToBSTR());
		return;
	}
	
	// vrscene attributes
	_vrsceneFile = getStrParam(pblock2, pb_file, t);
	_shadersFile = getStrParam(pblock2, pb_shaders_file, t);
	
	// cache attributes
	_crowdFields = getStrParam(pblock2, pb_crowd_fields, t);
	_cacheName = getStrParam(pblock2, pb_cache_name, t);
	_cacheDir = getStrParam(pblock2, pb_cache_dir, t);
	_characterFiles = getStrParam(pblock2, pb_character_files, t);

	// layout attributes
	_layoutEnable = pblock2->GetInt(pb_layout_enable, t) == 1;
	_layoutName = getStrParam(pblock2, pb_layout_name, t);
	_layoutDir = getStrParam(pblock2, pb_layout_dir, t);
	_terrainFile = getStrParam(pblock2, pb_terrain_file, t);

	// motion blur attributes
	BOOL overrideValue;
	node->GetUserPropBool(PROP_MOBLUR_OVERRIDEDURATION, overrideValue);
	_overMBlurWindowSize = overrideValue == 1;
	node->GetUserPropFloat(PROP_MOBLUR_DURATION, _mBlurWindowSize);
	node->GetUserPropBool(PROP_MOBLUR_USEDEFAULTGEOMSAMPLES, overrideValue);
	_overMBlurSamples = overrideValue == 0;
	node->GetUserPropInt(PROP_MOBLUR_GEOMSAMPLES, _mBlurSamples);
	_mBlurEnable = !(_overMBlurSamples && _mBlurSamples == 1); // moblur is disabled if the object geo samples == 1

	// culling attributes
	_frustumEnable = pblock2->GetInt(pb_frustum_enable, t) == 1;
	_frustumMargin = pblock2->GetFloat(pb_frustum_margin, t);
	_cameraMargin = pblock2->GetFloat(pb_camera_margin, t);

	// vray
	_frameOffset = pblock2->GetInt(pb_frame_offset, t);
	_defaultMaterial = getStrParam(pblock2, pb_default_material, t);
	_displayPercent = pblock2->GetFloat(pb_display_percentage, t);
	_instancingEnable = pblock2->GetInt(pb_instancing_enable, t) == 1;
	
	// object properties
	_objectIDBase=node->GetGBufID();
	_objectIDMode = pblock2->GetInt(pb_object_id_mode, t);
	_primaryVisibility=node->GetPrimaryVisibility()==1;
	_castsShadows=node->CastShadows()==1;

	// Get secondary visibility from the 3ds Max object properties
	_visibleInReflections=true;
	_visibleInRefractions=true;
	int nodeSecondaryVisibility=node->GetSecondaryVisibility();
	if (0==nodeSecondaryVisibility) _visibleInReflections=false;
	if (0==nodeSecondaryVisibility) _visibleInRefractions=false;

	// Check secondary visibility in the V-Ray object properties
	int vrayReflVisibility=true, vrayRefrVisibility=true;
	node->GetUserPropBool(PROP_GI_VISIBLETOREFL, vrayReflVisibility);
	node->GetUserPropBool(PROP_GI_VISIBLETOREFR, vrayRefrVisibility);

	if (!vrayReflVisibility) _visibleInReflections=false;
	if (!vrayRefrVisibility) _visibleInRefractions=false;

	// output
	_tempVRSceneFileDir = getStrParam(pblock2, pb_temp_vrscene_file_dir, t, "TEMP");
}

void VRayGolaem::wrapMaterial(Mtl *mtl) {
	if (!mtl)
		return;

	VR::VRenderMtl *vrenderMtl=VR::getVRenderMtl(mtl);
	if (!vrenderMtl)
		return; // Material is not V-Ray compatible, can't do anything.

	BRDFWrapper *wrapper=static_cast<BRDFWrapper*>(_vrayScene->newPluginWithoutParams(MTL_WRAPPER_VRAY_ID, NULL));
	if (!wrapper)
		return;

	wrapper->setMaxMtl(mtl, vrenderMtl, this);
}

void VRayGolaem::enumMaterials(Mtl *mtl) {
	if (!mtl || mtl->SuperClassID()!=MATERIAL_CLASS_ID)
		return;

	wrapMaterial(mtl);
	int numMtls=mtl->NumSubMtls();
	for (int i=0; i<numMtls; i++) {
		Mtl *sub=mtl->GetSubMtl(i);
		if (sub && sub->SuperClassID()==MATERIAL_CLASS_ID) {
			wrapMaterial(sub);
		}
	}
}

void VRayGolaem::createMaterials(VR::VRayCore *vray) {
	INode* node=getNode(this);
	if (NULL==node) {
		const VR::VRaySequenceData &sdata=vray->getSequenceData();
		if (sdata.progress) {
			const TCHAR *name_wstr=GetObjectName();
			GET_MBCS(name_wstr, name_mbcs);
			sdata.progress->warning("No node found for Golaem object \"%s\"; can't create materials", name_mbcs? name_mbcs : "<unknown>");
		}
		return;
	}

	enumMaterials(node->GetMtl());
}

class BRDFMaterialDesc: public PluginDesc {
public:
	PluginID getPluginID(void) VRAY_OVERRIDE {
		return MTL_WRAPPER_VRAY_ID;
	}

	Plugin* newPlugin(PluginHost *host) VRAY_OVERRIDE {
		return new BRDFWrapper;
	}

	void deletePlugin(Plugin *plugin) {
		delete static_cast<BRDFWrapper*>(plugin);
	}

	bool supportsInterface(InterfaceID id) {
		if (id==EXT_MATERIAL) return true;
		else if (id==EXT_BSDF) return true;
		else return false;
	}

	/// Returns the name of the plugin class (human readable name).
	tchar* getName(void) VRAY_OVERRIDE {
		return "MtlMaxWrapper";
	}
};

static BRDFMaterialDesc wrapperMaterialDesc;

//------------------------------------------------------------
// renderBegin / renderEnd
//------------------------------------------------------------
void VRayGolaem::renderBegin(TimeValue t, VR::VRayCore *_vray) 
{
	VR::VRayRenderer *vray=static_cast<VR::VRayRenderer*>(_vray);
	VRenderObject::renderBegin(t, vray);

	updateVRayParams(t);

	const VR::VRaySequenceData &sdata=vray->getSequenceData();

	if (!golaemPlugman) {
		golaemPlugman=newDefaultPluginManager();
		const tchar *vrayPluginPath = getVRayPluginPath();
		sdata.progress->info("VRayGolaem: Loading V-Ray plugins from \"%s\"", getVRayPluginPath());
		golaemPlugman->loadLibraryFromPathCollection(vrayPluginPath, "/vray_*.dll", NULL, vray->getSequenceData().progress);

		// Register our wrapper material.
		golaemPlugman->registerPlugin(&wrapperMaterialDesc);
	}

	// Creates the crowd .vrscene file on the fly if required
	VR::CharString vrSceneFileToLoad(_vrsceneFile);
	CStr outputDir(getenv (_tempVRSceneFileDir));
	if (outputDir!=NULL) 
	{
		if (outputDir.Length() != 0 && _cacheName.Length() != 0 && _crowdFields.length() != 0)
		{
			GET_MBCS(node->GetName(), nodeName);
			CStr outputPathStr(outputDir + "/" + _cacheName + "." + nodeName + ".vrscene");
			VR::CharString vrSceneExportPath(outputPathStr); // TODO
			if (!writeCrowdVRScene(vrSceneExportPath)) 
			{
				sdata.progress->warning("VRayGolaem: Error writing .vrscene file \"%s\"", vrSceneExportPath.ptr());
			}
			else 
			{
				sdata.progress->info("VRayGolaem: Writing .vrscene file \"%s\"", vrSceneExportPath.ptr());
				vrSceneFileToLoad = vrSceneExportPath;
			}
		}
		else
		{
			sdata.progress->warning("VRayGolaem: Node attributes invalid (CrowdFields, Cache Name or Cache Dir is empty)");
		}
	}
	else
	{
		sdata.progress->warning("VRayGolaem: Error finding environment variable for .vrscene output \"%s\"", _tempVRSceneFileDir);
	}

	// Load the .vrscene into the plugin manager
	_vrayScene=new VR::VRayScene(golaemPlugman);
	int prevNbPlugins(_vrayScene->getPluginManager()->enumPlugins(NULL));
	int newNbPlugins(prevNbPlugins);

	// Create wrapper plugins for all 3ds Max materials in the scene, so that the Golaem plugin can use them, if needed
	sdata.progress->info("VRayGolaem: Create materials attached to the VRayGolaem node");
	createMaterials(vray);
	newNbPlugins=_vrayScene->getPluginManager()->enumPlugins(NULL);
	sdata.progress->info("VRayGolaem: Materials created successfully, %i materials created", newNbPlugins-prevNbPlugins);
	prevNbPlugins = newNbPlugins;

	if (vrSceneFileToLoad.empty()) {
		sdata.progress->warning("VRayGolaem: No .vrscene file specified");
	} else {
		VR::ErrorCode errCode=_vrayScene->readFile(vrSceneFileToLoad.ptr());
		newNbPlugins=_vrayScene->getPluginManager()->enumPlugins(NULL);
		if (errCode.error()) {
			VR::CharString errMsg=errCode.getErrorString();
			sdata.progress->warning("VRayGolaem: Error loading .vrscene file \"%s\": %s", vrSceneFileToLoad.ptr(), errMsg.ptr());
		} else {
			sdata.progress->info("VRayGolaem: Scene file \"%s\" loaded successfully, %i nodes loaded", vrSceneFileToLoad.ptr(), newNbPlugins-prevNbPlugins);
		}
		prevNbPlugins = newNbPlugins;
	}

	if (_shadersFile.empty()) {
		sdata.progress->warning("VRayGolaem: No shaders .vrscene file specified");
	} else {
		VR::ErrorCode errCode=_vrayScene->readFile(_shadersFile.ptr());
		newNbPlugins=_vrayScene->getPluginManager()->enumPlugins(NULL);
		if (errCode.error()) {
			VR::CharString errMsg=errCode.getErrorString();
			sdata.progress->warning("VRayGolaem: Error loading shaders .vrscene file \"%s\": %s", _shadersFile.ptr(), errMsg.ptr());
		} else {
			sdata.progress->info("VRayGolaem: Shaders file \"%s\" loaded successfully, %i materials loaded", _shadersFile.ptr(), newNbPlugins-prevNbPlugins);
		}
		prevNbPlugins = newNbPlugins;
	}

	// check dependency files
	FindPluginOfTypeCallback pluginCallback(CROWDVRAYPLUGINID);
	_vrayScene->enumPlugins(&pluginCallback);
	if (pluginCallback._foundPlugins.length())
	{
		int frameOffset(0); // Todo
		
		// per crowdField
		VR::VRayPluginParameter* currentParam = NULL;
		CStr crowdField, cacheName, cacheDir, characterFiles;
		for (size_t iPlugin=0; iPlugin<pluginCallback._foundPlugins.length(); ++iPlugin)
		{
			VR::VRayPlugin* plugin (pluginCallback._foundPlugins[iPlugin]);
			currentParam = plugin->getParameter("glmCrowdField");
			if (currentParam) crowdField = currentParam->getString();
			currentParam = plugin->getParameter("glmCacheName");
			if (currentParam) cacheName = currentParam->getString();
			currentParam = plugin->getParameter("glmCacheFileDir");
			if (currentParam) cacheDir = currentParam->getString();
			currentParam = pluginCallback._foundPlugins[0]->getParameter("glmCharacterFiles");
			if (currentParam) characterFiles = currentParam->getString();

			int currentFrame = (int)((float)t / (float)TIME_TICKSPERSEC * (float)GetFrameRate()) + frameOffset; 
			CStr currentFrameStr; currentFrameStr.printf("%i", currentFrame);
			
			MaxSDK::Array<CStr> crowdFields;
			splitStr(crowdField, ';', crowdFields);
			for (size_t iCf=0, nbCf=crowdFields.length(); iCf<nbCf; ++iCf)
			{
			// caa
				CStr caaName (cacheDir + "/" + cacheName + "." + crowdFields[iCf] + ".caa");
			if (!fileExists(caaName)) sdata.progress->warning("VRayGolaem: Error loading Crowd Assets Association file \"%s\"", caaName);
			else sdata.progress->info("VRayGolaem: Crowd Assets Association file \"%s\" loaded successfully.", caaName);

			// gscs
				CStr gscsName (cacheDir + "/" + cacheName + "." + crowdFields[iCf] + ".gscs");
			if (!fileExists(gscsName)) sdata.progress->warning("VRayGolaem: Error loading Simulation Cache file \"%s\"", gscsName);
			else sdata.progress->info("VRayGolaem: Simulation Cache file \"%s\" loaded successfully.", gscsName);

			// gscf
				CStr gscfName (cacheDir + "/" + cacheName + "." + crowdFields[iCf] + "." + currentFrameStr +".gscf");
			if (!fileExists(gscfName)) sdata.progress->warning("VRayGolaem: Error loading Simulation Cache file \"%s\"", gscfName);
			else sdata.progress->info("VRayGolaem: Simulation Cache file \"%s\" loaded successfully.", gscfName);
			}

			// character files
			MaxSDK::Array<CStr> characters;
			splitStr(characterFiles, ';', characters);
			for (size_t iCh = 0, nbCh = characters.length(); iCh<nbCh; ++iCh)
			{
				if (!fileExists(characters[iCh])) sdata.progress->warning("VRayGolaem: Error loading Character file \"%s\"", characters[iCh]);
				else sdata.progress->info("VRayGolaem: Character file file \"%s\" loaded successfully.", characters[iCh]);
			}
		}
	}
	else
	{
		sdata.progress->warning("VRayGolaem: No GolaemCrowd node found in the current scene");
	}

	callRenderBegin(vray);
}

void VRayGolaem::renderEnd(VR::VRayCore *_vray) 
{
	VR::VRayRenderer *vray=static_cast<VR::VRayRenderer*>(_vray);
	VRenderObject::renderEnd(vray);
	
	callRenderEnd(vray);
	if (_vrayScene) {
		_vrayScene->freeMem();
		delete _vrayScene;
		_vrayScene=NULL;
	}
}

//------------------------------------------------------------
// frameBegin / frameEnd
//------------------------------------------------------------
void VRayGolaem::frameBegin(TimeValue t, VR::VRayCore *_vray) 
{
	VR::VRayRenderer *vray=static_cast<VR::VRayRenderer*>(_vray);
	VRenderObject::frameBegin(t, vray);
	callFrameBegin(vray);
}

void VRayGolaem::frameEnd(VR::VRayCore *_vray) 
{
	VR::VRayRenderer *vray=static_cast<VR::VRayRenderer*>(_vray);
	VRenderObject::frameEnd(vray);
	callFrameEnd(vray);
}

//------------------------------------------------------------
// newRenderInstance / deleteRenderInstance
//------------------------------------------------------------
VR::VRenderInstance* VRayGolaem::newRenderInstance(INode *node, VR::VRayCore *vray, int renderID) {
	if (vray) {
		const VR::VRaySequenceData &sdata=vray->getSequenceData();
		if (sdata.progress) {
			const TCHAR *nodeName=node? node->GetName() : _T("");
			GET_MBCS(nodeName, nodeName_mbcs);
			sdata.progress->debug("VRayGolaem: newRenderInstance() for node \"%s\"", nodeName_mbcs);
		}
	}
	VRayGolaemInstanceBase *golaemInstance=new VRayGolaemInstanceBase(this, node, vray, renderID);
	return golaemInstance;
}

void VRayGolaem::deleteRenderInstance(VR::VRenderInstance *ri) {
	delete static_cast<VRayGolaemInstanceBase*>(ri);
}

//------------------------------------------------------------
// callRenderBegin / callFrameBegin / callRenderEnd / callFrameEnd
//-----------------------------------------------------------
void VRayGolaem::callRenderBegin(VR::VRayCore *vray) {
	PluginRendererInterfaceRAII plgInterface(vray, this);

	PreRenderBeginCB preRenderBeginCb(vray);
	golaemPlugman->enumPlugins(&preRenderBeginCb);

	RenderBeginCB renderBeginCb(vray, 0.0f);
	golaemPlugman->enumPlugins(&renderBeginCb);
}

void VRayGolaem::callFrameBegin(VR::VRayCore *vray) {
	PluginRendererInterfaceRAII plgInterface(vray, this);
	TimeConversionRAII timeConversion(vray);

	PreFrameBeginCB preFrameBeginCb(vray);
	golaemPlugman->enumPlugins(&preFrameBeginCb);

	FrameBeginCB frameBeginCb(vray, vray->getFrameData().t);
	golaemPlugman->enumPlugins(&frameBeginCb);
}

void VRayGolaem::callRenderEnd(VR::VRayCore *vray) {
	PluginRendererInterfaceRAII plgInterface(vray, this);

	RenderEndCB renderEndCb(vray, 0.0f);
	golaemPlugman->enumPlugins(&renderEndCb);

	PostRenderEndCB postRenderEndCb(vray);
	golaemPlugman->enumPlugins(&postRenderEndCb);
}

void VRayGolaem::callFrameEnd(VR::VRayCore *vray) {
	PluginRendererInterfaceRAII plgInterface(vray, this);
	TimeConversionRAII timeConversion(vray);

	FrameEndCB frameEndCb(vray, vray->getFrameData().t);
	golaemPlugman->enumPlugins(&frameEndCb);

	PostFrameEndCB postFrameEndCb(vray);
	golaemPlugman->enumPlugins(&postFrameEndCb);
}

//------------------------------------------------------------
// compileGeometry / clearGeometry
//-----------------------------------------------------------
void VRayGolaem::compileGeometry(VR::VRayCore *vray) {
	TimeConversionRAII timeConversion(vray);

	const VR::VRaySequenceData &sdata=vray->getSequenceData();
	if (sdata.progress)
		sdata.progress->debug("VRayGolaem: Compiling geometry");

	CompileGeometryCB compileGeometryCb(vray);
	int res=golaemPlugman->enumPlugins(&compileGeometryCb);

	if (sdata.progress)
		sdata.progress->debug("VRayGolaem: %i plugins enumerated for compileGeometry()", res);
}

void VRayGolaem::clearGeometry(VR::VRayCore *vray) {
	TimeConversionRAII timeConversion(vray);

	const VR::VRaySequenceData &sdata=vray->getSequenceData();
	if (sdata.progress)
		sdata.progress->debug("VRayGolaem: Clearing geometry");

	ClearGeometryCB clearGeometryCb(vray);
	int res=golaemPlugman->enumPlugins(&clearGeometryCb);

	if (sdata.progress)
		sdata.progress->debug("VRayGolaem: %i plugins enumerated for clearGeometry()", res);
}

PluginManager* VRayGolaem::getPluginManager(void) {
	return golaemPlugman;
}

//************************************************************
// Read / Write VRScene
//************************************************************

//------------------------------------------------------------
// readCrowdVRScene: parse the imported crowd .vrscene to fill the node attributes
//------------------------------------------------------------
bool VRayGolaem::readCrowdVRScene(const VR::CharString& file) 
{	
	// check if this object is not an instance (then it has no max node to query)
	INode* node=getNode(this);
	if (node == NULL)
	{
		CStr logMessage = CStr("VRayGolaem: This object is an 3ds Max instance and is not supported. Please create a copy.");
		mprintf(logMessage.ToBSTR());
		return false;
	}
	
	// create a Vray context
	PluginManager* tempPlugMan(golaemPlugman);
	bool deletePlugMan(false);
	if (tempPlugMan == NULL)
	{
		tempPlugMan=newDefaultPluginManager();
		const tchar *vrayPluginPath = getVRayPluginPath();
		tempPlugMan->loadLibraryFromPathCollection(vrayPluginPath, "/vray_*.dll", NULL, NULL);
		deletePlugMan=true;
	}
	VR::VRayScene* tmpVrayScene=new VR::VRayScene(tempPlugMan);
	VR::ErrorCode errCode=tmpVrayScene->readFile(file.ptr());
	if (!errCode.error())
	{				
		// find the nodes
		FindPluginOfTypeCallback pluginCallback(CROWDVRAYPLUGINID);
		tempPlugMan->enumPlugins(&pluginCallback);

		// read attributes
		if (pluginCallback._foundPlugins.length())
		{
			VR::VRayPlugin* plugin (pluginCallback._foundPlugins[0]);
			VR::VRayPluginParameter* currentParam = NULL;
			CStr crowdFields;
			
			// transform
			currentParam = plugin->getParameter("glmTransform");
			if (currentParam)
			{
				VR::Transform t = currentParam->getTransform();
				Matrix3 transform(Point3(1, 0, 0), Point3(0, 1, 0), Point3(0, 0, 1), Point3(t.offs[0], t.offs[1], t.offs[2]));
				transform = transform * golaemToMax();

				// scale according to scene unit
				double scaleRatio (1. / GetMasterScale (UNITS_CENTIMETERS));
				transform.Scale(Point3(scaleRatio, scaleRatio, scaleRatio), true);

				node->SetNodeTM(0, transform);
			}

			// cache attributes
			currentParam = plugin->getParameter("glmCrowdField");
			if (currentParam)
			{
				crowdFields = currentParam->getString();
			}
			currentParam = plugin->getParameter("glmCacheName");
			if (currentParam)
			{
				GET_WSTR(currentParam->getString(), currentParamMbcs)
				pblock2->SetValue(pb_cache_name, 0, currentParamMbcs, 0);
			}
			currentParam = plugin->getParameter("glmCacheFileDir");
			if (currentParam)
			{
				GET_WSTR(currentParam->getString(), currentParamMbcs)
				pblock2->SetValue(pb_cache_dir, 0, currentParamMbcs, 0);
			}
			currentParam = plugin->getParameter("glmCharacterFiles");
			if (currentParam)
			{
				GET_WSTR(currentParam->getString(), currentParamMbcs)
				pblock2->SetValue(pb_character_files, 0, currentParamMbcs, 0);
			}

			// layout
			currentParam = plugin->getParameter("glmEnableLayout");
			if (currentParam) pblock2->SetValue(pb_layout_enable, 0, currentParam->getBool() == 1);
			currentParam = plugin->getParameter("glmLayoutName");
			if (currentParam)
			{
				GET_WSTR(currentParam->getString(), currentParamMbcs)
				pblock2->SetValue(pb_layout_name, 0, currentParamMbcs, 0);
			}
			currentParam = plugin->getParameter("glmLayoutDir");
			if (currentParam)
			{
				GET_WSTR(currentParam->getString(), currentParamMbcs)
				pblock2->SetValue(pb_layout_dir, 0, currentParamMbcs, 0);
			}
			currentParam = plugin->getParameter("glmTerrainFile");
			if (currentParam)
			{
				GET_WSTR(currentParam->getString(), currentParamMbcs)
				pblock2->SetValue(pb_terrain_file, 0, currentParamMbcs, 0);
			}

			// motion blur
			currentParam = plugin->getParameter("glmMBlurWindowSize");
			if (currentParam) 
			{
				node->SetUserPropBool(PROP_MOBLUR_OVERRIDEDURATION, true);
				node->SetUserPropFloat(PROP_MOBLUR_DURATION, currentParam->getFloat());
			}
			currentParam = plugin->getParameter("glmMBlurSamples");
			if (currentParam) 
			{
				node->SetUserPropBool(PROP_MOBLUR_USEDEFAULTGEOMSAMPLES, false);
				node->SetUserPropInt(PROP_MOBLUR_GEOMSAMPLES, currentParam->getInt());
			}
			// if motion blur is off, override geometry samples value with 1
			currentParam = plugin->getParameter("glmMBlurEnabled");
			if (currentParam)
			{
				if (currentParam->getInt() == 0)
				{
					node->SetUserPropBool(PROP_MOBLUR_USEDEFAULTGEOMSAMPLES, false);
					node->SetUserPropInt(PROP_MOBLUR_GEOMSAMPLES, 1);
				}
			}
		
			// frustum culling
			currentParam = plugin->getParameter("glmEnableFrustumCulling");
			if (currentParam) pblock2->SetValue(pb_frustum_enable, 0, currentParam->getBool() == 1);
			currentParam = plugin->getParameter("glmFrustumMargin");
			if (currentParam) pblock2->SetValue(pb_frustum_margin, 0, (float)currentParam->getDouble());
			currentParam = plugin->getParameter("glmCameraMargin");
			if (currentParam) pblock2->SetValue(pb_camera_margin, 0, (float)currentParam->getDouble());

			// vray
			currentParam = plugin->getParameter("glmRenderPercent");
			if (currentParam) pblock2->SetValue(pb_display_percentage, 0, currentParam->getFloat());
			currentParam = plugin->getParameter("glmFrameOffset");
			if (currentParam) pblock2->SetValue(pb_frame_offset, 0, currentParam->getInt());
			currentParam = plugin->getParameter("glmDefaultMaterial");
			if (currentParam)
			{
				GET_WSTR(currentParam->getString(), currentParamMbcs)
				pblock2->SetValue(pb_default_material, 0, currentParamMbcs, 0);
			}
			currentParam = plugin->getParameter("glmInstancingEnabled");
			if (currentParam) pblock2->SetValue(pb_instancing_enable, 0, currentParam->getBool() == 1);
			
			// properties (copy them in the max node as well if it exists)
			int objectIDBase(0);
			bool primaryVisibility(true), castShadows(true), inReflections(true), inRefractions(true);
			currentParam = plugin->getParameter("glmObjectIDBase");
			if (currentParam) objectIDBase = currentParam->getInt(); 
			currentParam = plugin->getParameter("glmObjectIDMode");
			if (currentParam) pblock2->SetValue(pb_object_id_mode, 0, currentParam->getInt());
			currentParam = plugin->getParameter("glmCameraVisibility");
			if (currentParam) primaryVisibility = currentParam->getBool() == 1;
			currentParam = plugin->getParameter("glmShadowsVisibility");
			if (currentParam) castShadows = currentParam->getBool() == 1;
			currentParam = plugin->getParameter("glmReflectionsVisibility");
			if (currentParam) inReflections = currentParam->getBool() == 1;
			currentParam = plugin->getParameter("glmRefractionsVisibility");
			if (currentParam) inRefractions = currentParam->getBool() == 1;

			node->SetGBufID(objectIDBase);
			node->SetPrimaryVisibility(primaryVisibility);
			node->SetCastShadows(castShadows);
			node->SetSecondaryVisibility(inReflections && inRefractions);

			int visibleInRefl((int) inReflections), visibleInRefr((int) inRefractions);
			node->SetUserPropBool(PROP_GI_VISIBLETOREFL, visibleInRefl);
			node->SetUserPropBool(PROP_GI_VISIBLETOREFR, visibleInRefr);
			
			// other crowdFields?
			for (size_t iPlugin=1; iPlugin<pluginCallback._foundPlugins.length(); ++iPlugin)
			{
				plugin = pluginCallback._foundPlugins[iPlugin];
				currentParam = plugin->getParameter("glmCrowdField");
				if (currentParam)
					crowdFields += (CStr(";") + CStr(currentParam->getString()));
			}
			GET_WSTR(crowdFields, currentParamMbcs)
			pblock2->SetValue(pb_crowd_fields, 0, currentParamMbcs, 0);

			// ok, vray_glmCrowdVRayPlugin.dll is loaded and all params are filled
			CStr logMessage = CStr("VRayGolaem: Success loading .vrscene file \"") + CStr(file.ptr()) + CStr("\" \n");
			mprintf(logMessage.ToBSTR());
		}
		else
		{
			// CROWDVRAYPLUGINID not found = not loaded or env not configured
			CStr vrayEnvVar = CStr(VRAYRT_PLUGINS) + CStr("_") + CStr(PROCESSOR_ARCHITECTURE);
			CStr logMessage = CStr("VRayGolaem: Error loading .vrscene file \"") + CStr(file.ptr()) + CStr("\". vray_glmCrowdVRayPlugin.dll plugin was not found in environment variable \"") + vrayEnvVar + CStr("\" (")+ CStr(getVRayPluginPath()) + CStr(").\n");
			mprintf(logMessage.ToBSTR());
		}
		
	}
	else
	{
		CStr logMessage = CStr("VRayGolaem: Success loading .vrscene file \"") + CStr(file.ptr()) + CStr("\". Vrscene file is invalid.\n");
		mprintf(logMessage.ToBSTR());
	}
	
	// delete the Vray context
	tmpVrayScene->freeMem();
	delete tmpVrayScene;

	if (deletePlugMan)
	{
		tempPlugMan->deleteAll();
		tempPlugMan->unloadAll();
		deleteDefaultPluginManager(tempPlugMan);
	}

	return true;
}

//------------------------------------------------------------
// writeCrowdVRScene: get the node attributes to create a crowd .vrscene
//------------------------------------------------------------
bool VRayGolaem::writeCrowdVRScene(const VR::CharString& file) 
{
	// check if this object is not an instance (then it has no max node to query)
	INode* node=getNode(this);
	if (node == NULL)
	{
		CStr logMessage = CStr("VRayGolaem: This object is an 3ds Max instance and is not supported. Please create a copy.");
		mprintf(logMessage.ToBSTR());
		return false;
	}
	GET_MBCS(node->GetName(), nodeName);
	Matrix3 transform = node->GetObjectTM(0) * maxToGolaem();
	
	// check file path
	std::stringstream outputStr;
	std::ofstream outputFileStream(file.ptr());
	if (!outputFileStream.is_open()) return false;

	// correct the name of the shader to call. When exporting a scene from Maya with Vray, some shader name special characters are replaced with not parsable character (":" => "__")
	// to be able to find the correct shader name to call, we need to apply the same conversion to the shader names contained in the cam file
	CStr correctedCacheName(_cacheName);
	convertToValidVrsceneName(_cacheName, correctedCacheName);

		// node
	outputStr << "Node " << correctedCacheName << nodeName << "@node" << std::endl;
		outputStr << "{" << std::endl;
		outputStr << "\t" << "transform=Transform(Matrix(Vector(1, 0, 0), Vector(0, 1, 0), Vector(0, 0, 1)), Vector(0, 0, 0));" << std::endl;
	outputStr << "\t" << "geometry=" << correctedCacheName << nodeName << "@mesh1;" << std::endl;
		outputStr << "\t" << "visible=1;" << std::endl;
		outputStr << "}" << std::endl;
		outputStr << std::endl;

	outputStr << "GolaemCrowd " << correctedCacheName << nodeName << "@mesh1" << std::endl;
		outputStr << "{" << std::endl;
		outputStr << "\t" << "glmTransform=Transform(Matrix(Vector("<< transform.GetRow(0)[0] <<", "<< transform.GetRow(0)[1] <<", "<< transform.GetRow(0)[2] <<")," << 
														   "Vector("<< transform.GetRow(1)[0] <<", "<< transform.GetRow(1)[1] <<", "<< transform.GetRow(1)[2] <<")," <<
														   "Vector("<< transform.GetRow(2)[0] <<", "<< transform.GetRow(2)[1] <<", "<< transform.GetRow(2)[2] <<"))," << 
														   "Vector("<< transform.GetRow(3)[0] <<", "<< transform.GetRow(3)[1] <<", "<< transform.GetRow(3)[2] <<"));" << std::endl;
		outputStr << "\t" << "glmFrameOffset="<< _frameOffset <<";" << std::endl;
		outputStr << "\t" << "glmCrowdField=\"" << _crowdFields << "\";" << std::endl;
		outputStr << "\t" << "glmCacheName=\"" << _cacheName << "\";" << std::endl;
		outputStr << "\t" << "glmCacheFileDir=\"" << _cacheDir << "\";" << std::endl;
		outputStr << "\t" << "glmCharacterFiles=\"" << _characterFiles << "\";" << std::endl;
		// layout
		outputStr << "\t" << "glmEnableLayout=" << _layoutEnable << ";" << std::endl;
		outputStr << "\t" << "glmLayoutName=\"" << _layoutName << "\";" << std::endl;
		outputStr << "\t" << "glmLayoutDir=\"" << _layoutDir << "\";" << std::endl;
		outputStr << "\t" << "glmTerrainFile=\"" << _terrainFile << "\";" << std::endl;
		// moblur
		outputStr << "\t" << "glmMBlurEnabled=" << _mBlurEnable << ";" << std::endl;
		if (_overMBlurWindowSize) outputStr << "\t" << "glmMBlurWindowSize=" << _mBlurWindowSize << ";" << std::endl;
		if (_overMBlurSamples) outputStr << "\t" << "glmMBlurSamples=" << _mBlurSamples << ";" << std::endl;
		// frustum culling
		outputStr << "\t" << "glmEnableFrustumCulling=" << _frustumEnable << ";" << std::endl;
		outputStr << "\t" << "glmFrustumMargin=" << _frustumMargin << ";" << std::endl;
		outputStr << "\t" << "glmCameraMargin=" << _cameraMargin << ";" << std::endl;
		// vray
		outputStr << "\t" << "glmDefaultMaterial=\""<< _defaultMaterial <<"\";" << std::endl;
		outputStr << "\t" << "glmObjectIDBase=" << _objectIDBase << ";" << std::endl;
		outputStr << "\t" << "glmObjectIDMode=" << _objectIDMode << ";" << std::endl;
		outputStr << "\t" << "glmRenderPercent=" << _displayPercent << ";" << std::endl;
		outputStr << "\t" << "glmInstancingEnabled=" << _instancingEnable << ";" << std::endl;
		outputStr << "\t" << "glmCameraVisibility=" << _primaryVisibility << ";" << std::endl;
		outputStr << "\t" << "glmShadowsVisibility=" << _castsShadows << ";" << std::endl;
		outputStr << "\t" << "glmReflectionsVisibility=" << _visibleInReflections << ";" << std::endl;
		outputStr << "\t" << "glmRefractionsVisibility=" << _visibleInRefractions << ";" << std::endl;

		outputStr << "\t" << "glmDccPackage=1;" << std::endl;

		outputStr << "}" << std::endl;
		outputStr << std::endl;

	// write in file
	outputFileStream << outputStr.str();
	outputFileStream.close();
	return true;
}

//************************************************************
// Inline utility functions
//************************************************************

//------------------------------------------------------------
// isCharInvalidVrscene
//------------------------------------------------------------
bool isCharInvalidVrscene(char c)
{
	if (c == '|' || c == '@') return false;
	if (c >= 'a' && c <= 'z') return false;
	if (c >= 'A' && c <= 'Z') return false;
	if (c >= '0' && c <= '9') return false;
	return true;
}

//------------------------------------------------------------
// convertToValidVrsceneName
//------------------------------------------------------------
void convertToValidVrsceneName(const CStr& strIn, CStr& strOut)
{
	int strSize = int(strIn.length());
	if (strSize == 0)
	{
		strOut.Resize(0);
		return;
	}
	strOut.Resize(strSize * 2);

	// If the first character is a digit, convert that to a letter
	int pos(0), i(0);
	strOut.dataForWrite()[0] = strIn[0];
	if (strIn[0] >= '0' && strIn[0] <= '9')
	{
		strOut.dataForWrite()[0] = 'a' + (strIn[0] - '0');
		pos++; 
		i++;
	}

	while (i < strSize)
	{
		if (isCharInvalidVrscene(strIn[i]))
		{
			strOut.dataForWrite()[pos++] = '_';
			if (strIn[i] == ':')
			{
				strOut.dataForWrite()[pos++] = '_';
			}
		}
		else strOut.dataForWrite()[pos++] = strIn[i];
		i++;
	}

	strOut.Resize(pos);
}

void splitStr(const CStr& input, char delim, MaxSDK::Array<CStr> & result)
{
	int startPos(0);
	if (input.length() == 0) return;
	
	// first character is delim
	if (input[0]==delim)
	{
		result.append("");
		startPos=1;
	}

	for (int iChar=1, nbChars=input.length(); iChar < nbChars; ++iChar)
	{
		if (input[iChar] == delim)
		{
			CStr tmpStr = input.Substr(startPos, iChar-startPos);
			result.append(tmpStr);
			startPos = iChar+1;
		}
	}

	if (startPos != input.length())
	{
		CStr tmpStr = input.Substr(startPos, input.length()-startPos);
		result.append(tmpStr);
	}
}


//************************************************************
// Inline draw functions
//************************************************************

inline void drawLine(GraphicsWindow *gw, const Point3 &p0, const Point3 &p1) 
{
	Point3 p[3]={ p0, p1 };
	gw->segment(p, TRUE);
}

inline void drawBBox(GraphicsWindow *gw, const Box3 &b) 
{
	gw->setTransform(Matrix3(1));
	Point3 p[8];
	for (int i=0; i<8; i++) p[i]=b[i];
	gw->startSegments();
	drawLine(gw, p[0], p[1]);
	drawLine(gw, p[0], p[2]);
	drawLine(gw, p[3], p[1]);
	drawLine(gw, p[3], p[2]);

	drawLine(gw, p[7], p[6]);
	drawLine(gw, p[7], p[5]);
	drawLine(gw, p[4], p[5]);
	drawLine(gw, p[4], p[6]);

	drawLine(gw, p[0], p[4]);
	drawLine(gw, p[1], p[5]);
	drawLine(gw, p[2], p[6]);
	drawLine(gw, p[3], p[7]);
	gw->endSegments();
}

inline void drawSphere(GraphicsWindow *gw, const Point3 &pos, float radius, int nsegs)
{
	float u0=radius, v0=0.0f;
	Point3 pt[3];

	// draw locator sphere
	gw->startSegments();
	for (int i=0; i<nsegs; i++) 
	{
		float a=2.0f*(float)pi*float(i+1)/float(nsegs);
		float u1=radius*cosf(a);
		float v1=radius*sinf(a);

		pt[0]=Point3(u0, v0, 0.0f) + pos;
		pt[1]=Point3(u1, v1, 0.0f) + pos;
		gw->segment(pt, true);

		pt[0]=Point3(0.0f, u0, v0) + pos;
		pt[1]=Point3(0.0f, u1, v1) + pos;
		gw->segment(pt, true);

		pt[0]=Point3(u0, 0.0f, v0) + pos;
		pt[1]=Point3(u1, 0.0f, v1) + pos;
		gw->segment(pt, true);

		u0=u1;
		v0=v1;
	}
	gw->endSegments();
}

inline void drawText(GraphicsWindow *gw, const MCHAR* text, const Point3& pos) 
{
	IPoint3 ipt;
	gw->wTransPoint(&pos, &ipt);

	// text position
	SIZE sp;
	gw->getTextExtents(text, &sp);

	// draw shadow text
	ipt.x-=sp.cx/2;
	ipt.y-=sp.cy/2;
	gw->setColor(TEXT_COLOR, 0.0f, 0.0f, 0.0f);
	gw->wText(&ipt, text);

	// draw white text
	ipt.x--;
	ipt.y--;
	gw->setColor(TEXT_COLOR, 1.0f, 1.0f, 1.0f);
	gw->wText(&ipt, text);
}

inline Matrix3 golaemToMax()
{
	return RotateXMatrix((float)pi/2);
}

inline Matrix3 maxToGolaem()
{
	return RotateXMatrix(-(float)pi/2);
}

// V-Ray materials expect rc.rayresult.sd to derive from VR::ShadeData, but this is not true for
// the geometry from Standalone plugins, so wrap the original shade data with this class. This also
// allows us to remap the texture mapping channels on the fly (in 3ds Max, they start from 1, but
// in the standalone plugins, they start from 0).
struct MtlShadeData: VR::ShadeData {
	MtlShadeData(VR::VRayContext &rc, VR::SurfaceProperties *surfaceProps, int mtlID, int rID, int objID) {
		renderID=rID;
		gbufID=mtlID;
		objectID=objID;
		orig_rc=&rc;
		orig_sd=rc.rayresult.sd;
		orig_sp=rc.rayresult.surfaceProps;
		rc.rayresult.sd=static_cast<VR::VRayShadeData*>(this);
		rc.rayresult.surfaceProps=surfaceProps;
		lastMapChannelIndex=-3;
	}
	~MtlShadeData(void) {
		orig_rc->rayresult.sd=orig_sd;
		orig_rc->rayresult.surfaceProps=orig_sp;
	}

	VR::Vector getUVWcoords(const VR::VRayContext &rc, int channel) {
		if (!initMapChannel(rc, channel))
			return VR::Vector(0.0f, 0.0f, 0.0f);
		return lastMapChannelTransform.offs;
	}

	void getUVWderivs(const VR::VRayContext &rc, int channel, VR::Vector derivs[2]) {
		if (!initMapChannel(rc, channel)) {
			derivs[0].makeZero();
			derivs[1].makeZero();
		} else {
			derivs[0]=rc.rayresult.dPdx*lastMapChannelTransform.m;
			derivs[1]=rc.rayresult.dPdy*lastMapChannelTransform.m;
		}
	}

	void getUVWbases(const VR::VRayContext &rc, int channel, VR::Vector bases[3]) {
		if (!initMapChannel(rc, channel)) {
			bases[0].makeZero();
			bases[1].makeZero();
			bases[2].makeZero();
		} else {
			bases[0]=lastMapChannelTransform.m[0];
			bases[1]=lastMapChannelTransform.m[1];
			bases[2]=lastMapChannelTransform.m[2];
		}
	}

	VR::Vector getUVWnormal(const VR::VRayContext &rc, int channel) {
		if (!initMapChannel(rc, channel)) {
			return VR::Vector(0.0f, 0.0f, 1.0f);
		} else {
			return crossf(lastMapChannelTransform.m[0], lastMapChannelTransform.m[1]);
		}
	}

	int getMtlID(const VR::VRayContext &rc) {
		VR::SurfaceInfoInterface *surfaceInfo=static_cast<VR::SurfaceInfoInterface*>(GET_INTERFACE(orig_sd, EXT_SURFACE_INFO));
		if (surfaceInfo)
			return surfaceInfo->getFaceID(rc);
		return 0;
	}
	int getGBufID(void) { return objectID; }
	int getSmoothingGroup(const VR::VRayContext &rc) { return 0; }
	int getEdgeVisibility(const VR::VRayContext &rc) { return 7; }

	int getSurfaceRenderID(const VR::VRayContext &rc) { return renderID; }
	int getMaterialRenderID(const VR::VRayContext &rc) { return gbufID; }

	PluginInterface* newInterface(InterfaceID id) {
		PluginInterface *res=orig_sd->newInterface(id);
		if (res)
			return res;
		return VR::ShadeData::newInterface(id);
	}
protected:
	int initMapChannel(const VR::VRayContext &rc, int channelIndex) {
		if (lastMapChannelIndex==channelIndex)
			return true;
		if (-2==lastMapChannelIndex)
			return false;
		VR::MappedSurface *mappedSurface=static_cast<VR::MappedSurface*>(GET_INTERFACE(orig_sd, EXT_MAPPED_SURFACE));
		if (!mappedSurface) {
			lastMapChannelIndex=-2;
			return false;
		}

		// In 3ds Max, mapping channels start from 1, so that's why we subtract 1 from the channelIndex here.
		lastMapChannelTransform=mappedSurface->getLocalUVWTransform(rc, channelIndex-1);
		lastMapChannelIndex=channelIndex;
		return true;
	}

	VR::VRayContext *orig_rc;
	VR::VRayShadeData *orig_sd;
	VR::VRaySurfaceProperties *orig_sp;
	int lastMapChannelIndex;
	VR::Transform lastMapChannelTransform;
	int gbufID, renderID, objectID;
};

void BRDFWrapper::shade(VR::VRayContext &rc) {
	// 3ds Max materials for V-Ray expect rc.rayresult.sd to be ShadeData, so create a wrapper here
	MtlShadeData shadeData(rc, NULL, mtlID, 0 /* renderID */, golaemInstance->getObjectID());
	VR::VRayInterface &vri=static_cast<VR::VRayInterface&>(rc);

	// Just call the original 3ds Max material to shade itself.
	vrayMtl->shade(vri, mtlID);

	// Handle alpha contribution - there's no one to do it for us since we don't go through VRayInstance::fullShade().
	if (rc.rayresult.surfaceProps && 0!=(rc.rayparams.localRayType & VR::RT_GBUFFER)) {
		float alphaContrib=static_cast<VR::SurfaceProperties*>(rc.rayresult.surfaceProps)->alphaContribution;
		if (alphaContrib>=0.0f) {
			rc.mtlresult.alpha*=alphaContrib;
			rc.mtlresult.alphaTransp=VR::Color(1.0f, 1.0f, 1.0f)*(1.0f-alphaContrib)+rc.mtlresult.alphaTransp*alphaContrib;
		} else {
			rc.mtlresult.alpha.makeZero();
			rc.mtlresult.alphaTransp=VR::Color(1.0f, 1.0f, 1.0f)*(1.0f+alphaContrib)-rc.mtlresult.alphaTransp*alphaContrib;
		}
	}
}

int BRDFWrapper::getMaterialRenderID(const VR::VRayContext &rc) {
	return mtlID;
}

int BRDFWrapper::isOpaque(void) {
#ifdef VRAY_MTLREQ_OPAQUE_SHADOWS
	return (maxMtlFlags & (VRAY_MTLREQ_OPAQUE_SHADOWS | MTLREQ_TRANSP))==VRAY_MTLREQ_OPAQUE_SHADOWS;
#else
	return false;
#endif
}

VR::BSDFSampler* BRDFWrapper::newBSDF(const VR::VRayContext &rc, VR::BSDFFlags flags) {
	if (!vrayMtl)
		return NULL;

	VR::VRenderMtlFlags mtlFlags;
	mtlFlags.force1sided=flags.force1sided;
	return vrayMtl->newBSDF(rc, mtlFlags);
}

void BRDFWrapper::deleteBSDF(const VR::VRayContext &rc, VR::BSDFSampler *bsdf) {
	if (!bsdf)
		return;

	vrayMtl->deleteBSDF(rc, bsdf);
}

void BRDFWrapper::setMaxMtl(Mtl *maxMtl, VR::VRenderMtl *vrayMtl, VRayGolaem *golaem) {
	this->maxMtl=maxMtl;
	this->vrayMtl=vrayMtl;
	this->golaemInstance=golaem;

	GET_MBCS(maxMtl->GetName(), mtlName);
	setPluginName(mtlName); // Set the name to be the same as the Max name, so that the Golaem plugin can find it.

	maxMtlFlags=maxMtl->Requirements(-1);
	mtlID=maxMtl->gbufID;
}

BRDFWrapper::BRDFWrapper(void):
	maxMtl(NULL),
	vrayMtl(NULL),
	maxMtlFlags(0),
	mtlID(0),
	golaemInstance(NULL)
{
}
