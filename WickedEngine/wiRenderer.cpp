#include "wiRenderer.h"
#include "wiFrameRate.h"
#include "wiHairParticle.h"
#include "wiEmittedParticle.h"
#include "wiResourceManager.h"
#include "wiSprite.h"
#include "wiSceneSystem.h"
#include "wiFrustum.h"
#include "wiRenderTarget.h"
#include "wiDepthTarget.h"
#include "wiHelper.h"
#include "wiMath.h"
#include "wiLensFlare.h"
#include "wiTextureHelper.h"
#include "wiPHYSICS.h"
#include "wiCube.h"
#include "wiEnums.h"
#include "wiRandom.h"
#include "wiFont.h"
#include "wiRectPacker.h"
#include "wiBackLog.h"
#include "wiProfiler.h"
#include "wiOcean.h"
#include "ShaderInterop_CloudGenerator.h"
#include "ShaderInterop_Skinning.h"
#include "ShaderInterop_TracedRendering.h"
#include "ShaderInterop_BVH.h"
#include "ShaderInterop_Utility.h"
#include "wiWidget.h"
#include "wiGPUSortLib.h"

#include <algorithm>

#include <DirectXCollision.h>

using namespace std;
using namespace wiGraphicsTypes;
using namespace wiSceneSystem;
using namespace wiECS;

#pragma region STATICS
GraphicsDevice* wiRenderer::graphicsDevice = nullptr;
Sampler				*wiRenderer::samplers[SSLOT_COUNT];
VertexShader		*wiRenderer::vertexShaders[VSTYPE_LAST];
PixelShader			*wiRenderer::pixelShaders[PSTYPE_LAST];
GeometryShader		*wiRenderer::geometryShaders[GSTYPE_LAST];
HullShader			*wiRenderer::hullShaders[HSTYPE_LAST];
DomainShader		*wiRenderer::domainShaders[DSTYPE_LAST];
ComputeShader		*wiRenderer::computeShaders[CSTYPE_LAST];
VertexLayout		*wiRenderer::vertexLayouts[VLTYPE_LAST];
RasterizerState		*wiRenderer::rasterizers[RSTYPE_LAST];
DepthStencilState	*wiRenderer::depthStencils[DSSTYPE_LAST];
BlendState			*wiRenderer::blendStates[BSTYPE_LAST];
GPUBuffer			*wiRenderer::constantBuffers[CBTYPE_LAST];
GPUBuffer			*wiRenderer::resourceBuffers[RBTYPE_LAST];
Texture				*wiRenderer::textures[TEXTYPE_LAST];
Sampler				*wiRenderer::customsamplers[SSTYPE_LAST];

GPURingBuffer		*wiRenderer::dynamicVertexBufferPool;

float wiRenderer::GAMMA = 2.2f;
int wiRenderer::SHADOWRES_2D = 1024, wiRenderer::SHADOWRES_CUBE = 256, wiRenderer::SHADOWCOUNT_2D = 5 + 3 + 3, wiRenderer::SHADOWCOUNT_CUBE = 5, wiRenderer::SOFTSHADOWQUALITY_2D = 2;
bool wiRenderer::HAIRPARTICLEENABLED=true,wiRenderer::EMITTERSENABLED=true;
bool wiRenderer::TRANSPARENTSHADOWSENABLED = false;
bool wiRenderer::ALPHACOMPOSITIONENABLED = false;
bool wiRenderer::wireRender = false, wiRenderer::debugSpheres = false, wiRenderer::debugBoneLines = false, wiRenderer::debugPartitionTree = false, wiRenderer::debugEmitters = false, wiRenderer::freezeCullingCamera = false
, wiRenderer::debugEnvProbes = false, wiRenderer::debugForceFields = false, wiRenderer::debugCameras = false, wiRenderer::gridHelper = false, wiRenderer::voxelHelper = false, wiRenderer::requestReflectionRendering = false, wiRenderer::advancedLightCulling = true
, wiRenderer::advancedRefractions = false;
bool wiRenderer::ldsSkinningEnabled = true;
float wiRenderer::SPECULARAA = 0.0f;
float wiRenderer::renderTime = 0, wiRenderer::renderTime_Prev = 0, wiRenderer::deltaTime = 0;
XMFLOAT2 wiRenderer::temporalAAJitter = XMFLOAT2(0, 0), wiRenderer::temporalAAJitterPrev = XMFLOAT2(0, 0);
float wiRenderer::RESOLUTIONSCALE = 1.0f;
GPUQuery wiRenderer::occlusionQueries[];
UINT wiRenderer::entityArrayOffset_Lights = 0, wiRenderer::entityArrayCount_Lights = 0;
UINT wiRenderer::entityArrayOffset_Decals = 0, wiRenderer::entityArrayCount_Decals = 0;
UINT wiRenderer::entityArrayOffset_ForceFields = 0, wiRenderer::entityArrayCount_ForceFields = 0;
UINT wiRenderer::entityArrayOffset_EnvProbes = 0, wiRenderer::entityArrayCount_EnvProbes = 0;

Entity wiRenderer::cameraID = INVALID_ENTITY;

Texture2D* wiRenderer::enviroMap = nullptr;
float wiRenderer::GameSpeed=1;
bool wiRenderer::debugLightCulling = false;
bool wiRenderer::occlusionCulling = false;
bool wiRenderer::temporalAA = false, wiRenderer::temporalAADEBUG = false;
wiRenderer::VoxelizedSceneData wiRenderer::voxelSceneData = VoxelizedSceneData();
PHYSICS* wiRenderer::physicsEngine = nullptr;
wiOcean* wiRenderer::ocean = nullptr;

Texture2D* shadowMapArray_2D = nullptr;
Texture2D* shadowMapArray_Cube = nullptr;
Texture2D* shadowMapArray_Transparent = nullptr;

string wiRenderer::SHADERPATH = "shaders/";
#pragma endregion

#pragma region STATIC TEMP

deque<wiSprite*> wiRenderer::waterRipples;

Scene* wiRenderer::scene = nullptr;

std::vector<pair<XMFLOAT4X4, XMFLOAT4>> wiRenderer::renderableBoxes;
std::vector<wiRenderer::RenderableLine> wiRenderer::renderableLines;

XMFLOAT4 wiRenderer::waterPlane = XMFLOAT4(0, 1, 0, 0);

wiSpinLock deferredMIPGenLock;
unordered_set<Texture2D*> deferredMIPGens;

#pragma endregion


struct FrameCulling
{
	Frustum frustum;
	CulledCollection culledRenderer_opaque;
	CulledCollection culledRenderer_transparent;
	vector<Entity> culledObjects;
	vector<Entity> culledLights;
	vector<Entity> culledDecals;
	vector<Entity> culledEnvProbes;

	void Clear()
	{
		culledRenderer_opaque.clear();
		culledRenderer_transparent.clear();
		culledObjects.clear();
		culledLights.clear();
		culledDecals.clear();
		culledEnvProbes.clear();
	}
};
unordered_map<CameraComponent*, FrameCulling> frameCullings;

GFX_STRUCT Instance
{
	XMFLOAT4A mat0;
	XMFLOAT4A mat1;
	XMFLOAT4A mat2;
	XMFLOAT4A color_dither; //rgb:color, a:dither

	Instance(){}
	Instance(const XMFLOAT4X4& matIn, const XMFLOAT4& color = XMFLOAT4(1, 1, 1, 1), float dither = 0){
		Create(matIn, color, dither);
	}
	inline void Create(const XMFLOAT4X4& matIn, const XMFLOAT4& color = XMFLOAT4(1, 1, 1, 1), float dither = 0) volatile
	{
		mat0.x = matIn._11;
		mat0.y = matIn._21;
		mat0.z = matIn._31;
		mat0.w = matIn._41;

		mat1.x = matIn._12;
		mat1.y = matIn._22;
		mat1.z = matIn._32;
		mat1.w = matIn._42;

		mat2.x = matIn._13;
		mat2.y = matIn._23;
		mat2.z = matIn._33;
		mat2.w = matIn._43;

		color_dither.x = color.x;
		color_dither.y = color.y;
		color_dither.z = color.z;
		color_dither.w = dither;
	}

	ALIGN_16
};
GFX_STRUCT InstancePrev
{
	XMFLOAT4A mat0;
	XMFLOAT4A mat1;
	XMFLOAT4A mat2;

	InstancePrev(){}
	InstancePrev(const XMFLOAT4X4& matIn)
	{
		Create(matIn);
	}
	inline void Create(const XMFLOAT4X4& matIn) volatile
	{
		mat0.x = matIn._11;
		mat0.y = matIn._21;
		mat0.z = matIn._31;
		mat0.w = matIn._41;

		mat1.x = matIn._12;
		mat1.y = matIn._22;
		mat1.z = matIn._32;
		mat1.w = matIn._42;

		mat2.x = matIn._13;
		mat2.y = matIn._23;
		mat2.z = matIn._33;
		mat2.w = matIn._43;
	}

	ALIGN_16
};
CBUFFER(MaterialCB, CBSLOT_RENDERER_MATERIAL)
{
	XMFLOAT4 baseColor; // + alpha (.w)
	XMFLOAT4 texMulAdd;
	float roughness;
	float reflectance;
	float metalness;
	float emissive;
	float refractionIndex;
	float subsurfaceScattering;
	float normalMapStrength;
	float parallaxOcclusionMapping;

	void Create(const MaterialComponent& mat)
	{
		baseColor = mat.baseColor;
		texMulAdd = mat.texMulAdd;
		roughness = mat.roughness;
		reflectance = mat.reflectance;
		metalness = mat.metalness;
		emissive = mat.emissive;
		refractionIndex = mat.refractionIndex;
		subsurfaceScattering = mat.subsurfaceScattering;
		normalMapStrength = (mat.normalMap == nullptr ? 0 : mat.normalMapStrength);
		parallaxOcclusionMapping = mat.parallaxOcclusionMapping;
	}
};

GPUBuffer impostorMaterialCB;
GPUBuffer impostorVB_POS;
GPUBuffer impostorVB_TEX;


void wiRenderer::Present(function<void()> drawToScreen1,function<void()> drawToScreen2,function<void()> drawToScreen3)
{
	GetDevice()->PresentBegin();
	
	if(drawToScreen1!=nullptr)
		drawToScreen1();
	if(drawToScreen2!=nullptr)
		drawToScreen2();
	if(drawToScreen3!=nullptr)
		drawToScreen3();

	GetDevice()->PresentEnd();

	OcclusionCulling_Read();

	wiFrameRate::Frame();

}


void wiRenderer::SetUpStaticComponents()
{
	for (int i = 0; i < VSTYPE_LAST; ++i)
	{
		SAFE_INIT(vertexShaders[i]);
	}
	for (int i = 0; i < PSTYPE_LAST; ++i)
	{
		SAFE_INIT(pixelShaders[i]);
	}
	for (int i = 0; i < GSTYPE_LAST; ++i)
	{
		SAFE_INIT(geometryShaders[i]);
	}
	for (int i = 0; i < HSTYPE_LAST; ++i)
	{
		SAFE_INIT(hullShaders[i]);
	}
	for (int i = 0; i < DSTYPE_LAST; ++i)
	{
		SAFE_INIT(domainShaders[i]);
	}
	for (int i = 0; i < CSTYPE_LAST; ++i)
	{
		SAFE_INIT(computeShaders[i]);
	}
	for (int i = 0; i < VLTYPE_LAST; ++i)
	{
		SAFE_INIT(vertexLayouts[i]);
	}
	for (int i = 0; i < RSTYPE_LAST; ++i)
	{
		SAFE_INIT(rasterizers[i]);
	}
	for (int i = 0; i < DSSTYPE_LAST; ++i)
	{
		SAFE_INIT(depthStencils[i]);
	}
	for (int i = 0; i < CBTYPE_LAST; ++i)
	{
		SAFE_INIT(constantBuffers[i]);
	}
	for (int i = 0; i < RBTYPE_LAST; ++i)
	{
		SAFE_INIT(resourceBuffers[i]);
	}
	for (int i = 0; i < TEXTYPE_LAST; ++i)
	{
		SAFE_INIT(textures[i]);
	}
	for (int i = 0; i < SSLOT_COUNT_PERSISTENT; ++i)
	{
		SAFE_INIT(samplers[i]);
	}
	for (int i = 0; i < SSTYPE_LAST; ++i)
	{
		SAFE_INIT(customsamplers[i]);
	}

	cameraID = CreateEntity();

	wireRender=false;
	debugSpheres=false;

	SetUpStates();
	LoadBuffers();
	LoadShaders();
	
	wiHairParticle::SetUpStatic();
	wiEmittedParticle::SetUpStatic();

	Cube::LoadStatic();

	SetShadowProps2D(SHADOWRES_2D, SHADOWCOUNT_2D, SOFTSHADOWQUALITY_2D);
	SetShadowPropsCube(SHADOWRES_CUBE, SHADOWCOUNT_CUBE);

	// Set up impostor resources:
	{
		if (!impostorMaterialCB.IsValid())
		{
			MaterialCB mcb;
			ZeroMemory(&mcb, sizeof(mcb));
			mcb.baseColor = XMFLOAT4(1, 1, 1, 1);
			mcb.texMulAdd = XMFLOAT4(1, 1, 0, 0);
			mcb.normalMapStrength = 1.0f;
			mcb.roughness = 1.0f;
			mcb.reflectance = 1.0f;
			mcb.metalness = 1.0f;

			GPUBufferDesc bd;
			bd.BindFlags = BIND_CONSTANT_BUFFER;
			bd.Usage = USAGE_IMMUTABLE;
			bd.CPUAccessFlags = 0;
			bd.ByteWidth = sizeof(MaterialCB);
			SubresourceData initData;
			initData.pSysMem = &mcb;

			GetDevice()->CreateBuffer(&bd, &initData, &impostorMaterialCB);
		}
		if (!impostorVB_POS.IsValid())
		{
			const int count = 6 * 6;

			XMFLOAT3 impostorVertices_pos[count];
			XMFLOAT3 impostorVertices_nor[count];
			XMFLOAT2 impostorVertices_tex[count];

			float stepX = 1.f / 6.f;

			// front
			impostorVertices_pos[0] = XMFLOAT3(-1, 1, 0);
			impostorVertices_nor[0] = XMFLOAT3(0, 0, -1);
			impostorVertices_tex[0] = XMFLOAT2(0, 0);

			impostorVertices_pos[1] = XMFLOAT3(-1, -1, 0);
			impostorVertices_nor[1] = XMFLOAT3(0, 0, -1);
			impostorVertices_tex[1] = XMFLOAT2(0, 1);

			impostorVertices_pos[2] = XMFLOAT3(1, 1, 0);
			impostorVertices_nor[2] = XMFLOAT3(0, 0, -1);
			impostorVertices_tex[2] = XMFLOAT2(stepX, 0);

			impostorVertices_pos[3] = XMFLOAT3(-1, -1, 0);
			impostorVertices_nor[3] = XMFLOAT3(0, 0, -1);
			impostorVertices_tex[3] = XMFLOAT2(0, 1);

			impostorVertices_pos[4] = XMFLOAT3(1, -1, 0);
			impostorVertices_nor[4] = XMFLOAT3(0, 0, -1);
			impostorVertices_tex[4] = XMFLOAT2(stepX, 1);

			impostorVertices_pos[5] = XMFLOAT3(1, 1, 0);
			impostorVertices_nor[5] = XMFLOAT3(0, 0, -1);
			impostorVertices_tex[5] = XMFLOAT2(stepX, 0);

			// right
			impostorVertices_pos[6] = XMFLOAT3(0, 1, -1);
			impostorVertices_nor[6] = XMFLOAT3(1, 0, 0);
			impostorVertices_tex[6] = XMFLOAT2(stepX, 0);

			impostorVertices_pos[7] = XMFLOAT3(0, -1, -1);
			impostorVertices_nor[7] = XMFLOAT3(1, 0, 0);
			impostorVertices_tex[7] = XMFLOAT2(stepX, 1);

			impostorVertices_pos[8] = XMFLOAT3(0, 1, 1);
			impostorVertices_nor[8] = XMFLOAT3(1, 0, 0);
			impostorVertices_tex[8] = XMFLOAT2(stepX * 2, 0);

			impostorVertices_pos[9] = XMFLOAT3(0, -1, -1);
			impostorVertices_nor[9] = XMFLOAT3(1, 0, 0);
			impostorVertices_tex[9] = XMFLOAT2(stepX, 1);

			impostorVertices_pos[10] = XMFLOAT3(0, -1, 1);
			impostorVertices_nor[10] = XMFLOAT3(1, 0, 0);
			impostorVertices_tex[10] = XMFLOAT2(stepX * 2, 1);

			impostorVertices_pos[11] = XMFLOAT3(0, 1, 1);
			impostorVertices_nor[11] = XMFLOAT3(1, 0, 0);
			impostorVertices_tex[11] = XMFLOAT2(stepX * 2, 0);

			// back
			impostorVertices_pos[12] = XMFLOAT3(-1, 1, 0);
			impostorVertices_nor[12] = XMFLOAT3(0, 0, -1);
			impostorVertices_tex[12] = XMFLOAT2(stepX * 3, 0);

			impostorVertices_pos[13] = XMFLOAT3(1, 1, 0);
			impostorVertices_nor[13] = XMFLOAT3(0, 0, -1);
			impostorVertices_tex[13] = XMFLOAT2(stepX * 2, 0);

			impostorVertices_pos[14] = XMFLOAT3(-1, -1, 0);
			impostorVertices_nor[14] = XMFLOAT3(0, 0, -1);
			impostorVertices_tex[14] = XMFLOAT2(stepX * 3, 1);

			impostorVertices_pos[15] = XMFLOAT3(-1, -1, 0);
			impostorVertices_nor[15] = XMFLOAT3(0, 0, -1);
			impostorVertices_tex[15] = XMFLOAT2(stepX * 3, 1);

			impostorVertices_pos[16] = XMFLOAT3(1, 1, 0);
			impostorVertices_nor[16] = XMFLOAT3(0, 0, -1);
			impostorVertices_tex[16] = XMFLOAT2(stepX * 2, 0);

			impostorVertices_pos[17] = XMFLOAT3(1, -1, 0);
			impostorVertices_nor[17] = XMFLOAT3(0, 0, -1);
			impostorVertices_tex[17] = XMFLOAT2(stepX * 2, 1);

			// left
			impostorVertices_pos[18] = XMFLOAT3(0, 1, -1);
			impostorVertices_nor[18] = XMFLOAT3(1, 0, 0);
			impostorVertices_tex[18] = XMFLOAT2(stepX * 4, 0);

			impostorVertices_pos[19] = XMFLOAT3(0, 1, 1);
			impostorVertices_nor[19] = XMFLOAT3(1, 0, 0);
			impostorVertices_tex[19] = XMFLOAT2(stepX * 3, 0);

			impostorVertices_pos[20] = XMFLOAT3(0, -1, -1);
			impostorVertices_nor[20] = XMFLOAT3(1, 0, 0);
			impostorVertices_tex[20] = XMFLOAT2(stepX * 4, 1);

			impostorVertices_pos[21] = XMFLOAT3(0, -1, -1);
			impostorVertices_nor[21] = XMFLOAT3(1, 0, 0);
			impostorVertices_tex[21] = XMFLOAT2(stepX * 4, 1);

			impostorVertices_pos[22] = XMFLOAT3(0, 1, 1);
			impostorVertices_nor[22] = XMFLOAT3(1, 0, 0);
			impostorVertices_tex[22] = XMFLOAT2(stepX * 3, 0);

			impostorVertices_pos[23] = XMFLOAT3(0, -1, 1);
			impostorVertices_nor[23] = XMFLOAT3(1, 0, 0);
			impostorVertices_tex[23] = XMFLOAT2(stepX * 3, 1);

			// bottom
			impostorVertices_pos[24] = XMFLOAT3(-1, 0, 1);
			impostorVertices_nor[24] = XMFLOAT3(0, 1, 0);
			impostorVertices_tex[24] = XMFLOAT2(stepX * 4, 0);

			impostorVertices_pos[25] = XMFLOAT3(1, 0, 1);
			impostorVertices_nor[25] = XMFLOAT3(0, 1, 0);
			impostorVertices_tex[25] = XMFLOAT2(stepX * 5, 0);

			impostorVertices_pos[26] = XMFLOAT3(-1, 0, -1);
			impostorVertices_nor[26] = XMFLOAT3(0, 1, 0);
			impostorVertices_tex[26] = XMFLOAT2(stepX * 4, 1);

			impostorVertices_pos[27] = XMFLOAT3(-1, 0, -1);
			impostorVertices_nor[27] = XMFLOAT3(0, 1, 0);
			impostorVertices_tex[27] = XMFLOAT2(stepX * 4, 1);

			impostorVertices_pos[28] = XMFLOAT3(1, 0, 1);
			impostorVertices_nor[28] = XMFLOAT3(0, 1, 0);
			impostorVertices_tex[28] = XMFLOAT2(stepX * 5, 0);

			impostorVertices_pos[29] = XMFLOAT3(1, 0, -1);
			impostorVertices_nor[29] = XMFLOAT3(0, 1, 0);
			impostorVertices_tex[29] = XMFLOAT2(stepX * 5, 1);

			// top
			impostorVertices_pos[30] = XMFLOAT3(-1, 0, 1);
			impostorVertices_nor[30] = XMFLOAT3(0, 1, 0);
			impostorVertices_tex[30] = XMFLOAT2(stepX * 5, 0);

			impostorVertices_pos[31] = XMFLOAT3(-1, 0, -1);
			impostorVertices_nor[31] = XMFLOAT3(0, 1, 0);
			impostorVertices_tex[31] = XMFLOAT2(stepX * 5, 1);

			impostorVertices_pos[32] = XMFLOAT3(1, 0, 1);
			impostorVertices_nor[32] = XMFLOAT3(0, 1, 0);
			impostorVertices_tex[32] = XMFLOAT2(stepX * 6, 0);

			impostorVertices_pos[33] = XMFLOAT3(-1, 0, -1);
			impostorVertices_nor[33] = XMFLOAT3(0, 1, 0);
			impostorVertices_tex[33] = XMFLOAT2(stepX * 5, 1);

			impostorVertices_pos[34] = XMFLOAT3(1, 0, -1);
			impostorVertices_nor[34] = XMFLOAT3(0, 1, 0);
			impostorVertices_tex[34] = XMFLOAT2(stepX * 6, 1);

			impostorVertices_pos[35] = XMFLOAT3(1, 0, 1);
			impostorVertices_nor[35] = XMFLOAT3(0, 1, 0);
			impostorVertices_tex[35] = XMFLOAT2(stepX * 6, 0);


			MeshComponent::Vertex_POS impostorVertices_POS[count];
			MeshComponent::Vertex_TEX impostorVertices_TEX[count];
			for (int i = 0; i < count; ++i)
			{
				impostorVertices_POS[i].FromFULL(impostorVertices_pos[i], impostorVertices_nor[i], 0);
				impostorVertices_TEX[i].FromFULL(impostorVertices_tex[i]);
			}


			GPUBufferDesc bd;
			ZeroMemory(&bd, sizeof(bd));
			bd.Usage = USAGE_IMMUTABLE;
			bd.BindFlags = BIND_VERTEX_BUFFER;
			bd.CPUAccessFlags = 0;
			SubresourceData InitData;
			ZeroMemory(&InitData, sizeof(InitData));
			InitData.pSysMem = impostorVertices_POS;
			bd.ByteWidth = sizeof(impostorVertices_POS);
			wiRenderer::GetDevice()->CreateBuffer(&bd, &InitData, &impostorVB_POS);
			InitData.pSysMem = impostorVertices_TEX;
			bd.ByteWidth = sizeof(impostorVertices_TEX);
			wiRenderer::GetDevice()->CreateBuffer(&bd, &InitData, &impostorVB_TEX);
		}
	}
}
void wiRenderer::CleanUpStatic()
{

	wiHairParticle::CleanUpStatic();
	wiEmittedParticle::CleanUpStatic();
	Cube::CleanUpStatic();


	for (int i = 0; i < VSTYPE_LAST; ++i)
	{
		SAFE_DELETE(vertexShaders[i]);
	}
	for (int i = 0; i < PSTYPE_LAST; ++i)
	{
		SAFE_DELETE(pixelShaders[i]);
	}
	for (int i = 0; i < GSTYPE_LAST; ++i)
	{
		SAFE_DELETE(geometryShaders[i]);
	}
	for (int i = 0; i < HSTYPE_LAST; ++i)
	{
		SAFE_DELETE(hullShaders[i]);
	}
	for (int i = 0; i < DSTYPE_LAST; ++i)
	{
		SAFE_DELETE(domainShaders[i]);
	}
	for (int i = 0; i < CSTYPE_LAST; ++i)
	{
		SAFE_DELETE(computeShaders[i]);
	}
	for (int i = 0; i < VLTYPE_LAST; ++i)
	{
		SAFE_DELETE(vertexLayouts[i]);
	}
	for (int i = 0; i < RSTYPE_LAST; ++i)
	{
		SAFE_DELETE(rasterizers[i]);
	}
	for (int i = 0; i < DSSTYPE_LAST; ++i)
	{
		SAFE_DELETE(depthStencils[i]);
	}
	for (int i = 0; i < BSTYPE_LAST; ++i)
	{
		SAFE_DELETE(blendStates[i]);
	}
	for (int i = 0; i < CBTYPE_LAST; ++i)
	{
		SAFE_DELETE(constantBuffers[i]);
	}
	for (int i = 0; i < RBTYPE_LAST; ++i)
	{
		SAFE_DELETE(resourceBuffers[i]);
	}
	for (int i = 0; i < TEXTYPE_LAST; ++i)
	{
		SAFE_DELETE(textures[i]);
	}
	for (int i = 0; i < SSLOT_COUNT_PERSISTENT; ++i)
	{
		SAFE_DELETE(samplers[i]);
	}
	for (int i = 0; i < SSTYPE_LAST; ++i)
	{
		SAFE_DELETE(customsamplers[i]);
	}

	SAFE_DELETE(dynamicVertexBufferPool);

	if (physicsEngine) physicsEngine->CleanUp();

	SAFE_DELETE(graphicsDevice);
}
void wiRenderer::ClearWorld()
{
	GetDevice()->WaitForGPU();
	
	if (physicsEngine)
		physicsEngine->ClearWorld();

	enviroMap = nullptr;

	for (wiSprite* x : waterRipples)
		x->CleanUp();
	waterRipples.clear();

	GetScene().Clear();

	deferredMIPGenLock.lock();
	deferredMIPGens.clear();
	deferredMIPGenLock.unlock();
}
float wiRenderer::GetGameSpeed() { return GameSpeed; }


bool wiRenderer::ResolutionChanged()
{
	//detect internal resolution change:
	static float _savedresscale = GetResolutionScale();
	static uint64_t lastFrameInternalResChange = 0;
	if (_savedresscale != GetResolutionScale() || lastFrameInternalResChange == GetDevice()->GetFrameCount())
	{
		_savedresscale = GetResolutionScale();
		lastFrameInternalResChange = GetDevice()->GetFrameCount();
		return true;
	}

	// detect device resolution change:
	return GetDevice()->ResolutionChanged();
}


void wiRenderer::LoadBuffers()
{
	GPUBufferDesc bd;

	// Ring buffer allows fast allocation of dynamic buffers for one frame:
	dynamicVertexBufferPool = new GPURingBuffer;
	bd.BindFlags = BIND_VERTEX_BUFFER;
	bd.ByteWidth = 1024 * 1024 * 64;
	bd.Usage = USAGE_DYNAMIC;
	bd.CPUAccessFlags = CPU_ACCESS_WRITE;
	bd.MiscFlags = 0;
	GetDevice()->CreateBuffer(&bd, nullptr, dynamicVertexBufferPool);
	GetDevice()->SetName(dynamicVertexBufferPool, "DynamicVertexBufferPool");


	for (int i = 0; i < CBTYPE_LAST; ++i)
	{
		constantBuffers[i] = new GPUBuffer;
	}

	ZeroMemory( &bd, sizeof(bd) );
	bd.BindFlags = BIND_CONSTANT_BUFFER;

	//Persistent buffers...

	// Per World Constant buffer will be updated occasionally, so it should reside in DEFAULT GPU memory!
	bd.CPUAccessFlags = 0;
	bd.Usage = USAGE_DEFAULT;
	bd.ByteWidth = sizeof(WorldCB);
	GetDevice()->CreateBuffer(&bd, nullptr, constantBuffers[CBTYPE_WORLD]);
	GetDevice()->SetName(constantBuffers[CBTYPE_WORLD], "PerWorldConstantBuffer");

	bd.ByteWidth = sizeof(FrameCB);
	GetDevice()->CreateBuffer(&bd, nullptr, constantBuffers[CBTYPE_FRAME]);
	GetDevice()->SetName(constantBuffers[CBTYPE_FRAME], "PerFrameConstantBuffer");

	// The other constant buffers will be updated frequently (> per frame) so they should reside in DYNAMIC GPU memory!
	bd.Usage = USAGE_DYNAMIC;
	bd.CPUAccessFlags = CPU_ACCESS_WRITE;

	bd.ByteWidth = sizeof(CameraCB);
	GetDevice()->CreateBuffer(&bd, nullptr, constantBuffers[CBTYPE_CAMERA]);
	GetDevice()->SetName(constantBuffers[CBTYPE_FRAME], "PerFrameConstantBuffer");

	bd.ByteWidth = sizeof(MiscCB);
	GetDevice()->CreateBuffer(&bd, nullptr, constantBuffers[CBTYPE_MISC]);
	GetDevice()->SetName(constantBuffers[CBTYPE_MISC], "MiscConstantBuffer");

	bd.ByteWidth = sizeof(APICB);
	GetDevice()->CreateBuffer(&bd, nullptr, constantBuffers[CBTYPE_API]);
	GetDevice()->SetName(constantBuffers[CBTYPE_API], "APIConstantBuffer");


	// On demand buffers...

	bd.ByteWidth = sizeof(VolumeLightCB);
	GetDevice()->CreateBuffer(&bd, nullptr, constantBuffers[CBTYPE_VOLUMELIGHT]);

	bd.ByteWidth = sizeof(DecalCB);
	GetDevice()->CreateBuffer(&bd, nullptr, constantBuffers[CBTYPE_DECAL]);

	bd.ByteWidth = sizeof(CubeMapRenderCB);
	GetDevice()->CreateBuffer(&bd, nullptr, constantBuffers[CBTYPE_CUBEMAPRENDER]);

	bd.ByteWidth = sizeof(TessellationCB);
	GetDevice()->CreateBuffer(&bd, nullptr, constantBuffers[CBTYPE_TESSELLATION]);

	bd.ByteWidth = sizeof(DispatchParamsCB);
	GetDevice()->CreateBuffer(&bd, nullptr, constantBuffers[CBTYPE_DISPATCHPARAMS]);

	bd.ByteWidth = sizeof(CloudGeneratorCB);
	GetDevice()->CreateBuffer(&bd, nullptr, constantBuffers[CBTYPE_CLOUDGENERATOR]);

	bd.ByteWidth = sizeof(TracedRenderingCB);
	GetDevice()->CreateBuffer(&bd, nullptr, constantBuffers[CBTYPE_RAYTRACE]);

	bd.ByteWidth = sizeof(BVHCB);
	GetDevice()->CreateBuffer(&bd, nullptr, constantBuffers[CBTYPE_BVH]);

	bd.ByteWidth = sizeof(GenerateMIPChainCB);
	GetDevice()->CreateBuffer(&bd, nullptr, constantBuffers[CBTYPE_MIPGEN]);

	bd.ByteWidth = sizeof(FilterEnvmapCB);
	GetDevice()->CreateBuffer(&bd, nullptr, constantBuffers[CBTYPE_FILTERENVMAP]);

	bd.ByteWidth = sizeof(CopyTextureCB);
	GetDevice()->CreateBuffer(&bd, nullptr, constantBuffers[CBTYPE_COPYTEXTURE]);





	// Resource Buffers:

	for (int i = 0; i < RBTYPE_LAST; ++i)
	{
		resourceBuffers[i] = new GPUBuffer;
	}

	bd.Usage = USAGE_DEFAULT;
	bd.CPUAccessFlags = 0;


	bd.ByteWidth = sizeof(ShaderEntityType) * MAX_SHADER_ENTITY_COUNT;
	bd.BindFlags = BIND_SHADER_RESOURCE;
	bd.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
	bd.StructureByteStride = sizeof(ShaderEntityType);
	GetDevice()->CreateBuffer(&bd, nullptr, resourceBuffers[RBTYPE_ENTITYARRAY]);

	bd.ByteWidth = sizeof(XMMATRIX) * MATRIXARRAY_COUNT;
	bd.BindFlags = BIND_SHADER_RESOURCE;
	bd.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
	bd.StructureByteStride = sizeof(XMMATRIX);
	GetDevice()->CreateBuffer(&bd, nullptr, resourceBuffers[RBTYPE_MATRIXARRAY]);

	SAFE_DELETE(resourceBuffers[RBTYPE_VOXELSCENE]); // lazy init on request
}

enum OBJECTRENDERING_DOUBLESIDED
{
	OBJECTRENDERING_DOUBLESIDED_DISABLED,
	OBJECTRENDERING_DOUBLESIDED_ENABLED,
	OBJECTRENDERING_DOUBLESIDED_COUNT
};
enum OBJECTRENDERING_TESSELLATION
{
	OBJECTRENDERING_TESSELLATION_DISABLED,
	OBJECTRENDERING_TESSELLATION_ENABLED,
	OBJECTRENDERING_TESSELLATION_COUNT
};
enum OBJECTRENDERING_ALPHATEST
{
	OBJECTRENDERING_ALPHATEST_DISABLED,
	OBJECTRENDERING_ALPHATEST_ENABLED,
	OBJECTRENDERING_ALPHATEST_COUNT
};
enum OBJECTRENDERING_TRANSPARENCY
{
	OBJECTRENDERING_TRANSPARENCY_DISABLED,
	OBJECTRENDERING_TRANSPARENCY_ENABLED,
	OBJECTRENDERING_TRANSPARENCY_COUNT
};
enum OBJECTRENDERING_NORMALMAP
{
	OBJECTRENDERING_NORMALMAP_DISABLED,
	OBJECTRENDERING_NORMALMAP_ENABLED,
	OBJECTRENDERING_NORMALMAP_COUNT
};
enum OBJECTRENDERING_PLANARREFLECTION
{
	OBJECTRENDERING_PLANARREFLECTION_DISABLED,
	OBJECTRENDERING_PLANARREFLECTION_ENABLED,
	OBJECTRENDERING_PLANARREFLECTION_COUNT
};
enum OBJECTRENDERING_POM
{
	OBJECTRENDERING_POM_DISABLED,
	OBJECTRENDERING_POM_ENABLED,
	OBJECTRENDERING_POM_COUNT
};
GraphicsPSO* PSO_object[SHADERTYPE_COUNT][OBJECTRENDERING_DOUBLESIDED_COUNT][OBJECTRENDERING_TESSELLATION_COUNT][OBJECTRENDERING_ALPHATEST_COUNT][OBJECTRENDERING_TRANSPARENCY_COUNT][OBJECTRENDERING_NORMALMAP_COUNT][OBJECTRENDERING_PLANARREFLECTION_COUNT][OBJECTRENDERING_POM_COUNT] = {};
GraphicsPSO* PSO_object_water[SHADERTYPE_COUNT] = {};
GraphicsPSO* PSO_object_wire = nullptr;
GraphicsPSO* GetObjectPSO(SHADERTYPE shaderType, bool doublesided, bool tessellation, const MaterialComponent& material, bool forceAlphaTestForDithering)
{
	if (wiRenderer::IsWireRender())
	{
		switch (shaderType)
		{
		case SHADERTYPE_TEXTURE:
		case SHADERTYPE_DEFERRED:
		case SHADERTYPE_FORWARD:
		case SHADERTYPE_TILEDFORWARD:
			return PSO_object_wire;
		}
		return nullptr;
	}

	if (material.IsWater())
	{
		return PSO_object_water[shaderType];
	}

	bool alphatest = material.IsAlphaTestEnabled() || forceAlphaTestForDithering;
	bool transparent = material.IsTransparent();
	bool normalmap = material.GetNormalMap() != nullptr;
	bool planarreflection = material.HasPlanarReflection();
	bool pom = material.parallaxOcclusionMapping > 0;

	return PSO_object[shaderType][doublesided][tessellation][alphatest][transparent][normalmap][planarreflection][pom];
}


VLTYPES GetVLTYPE(SHADERTYPE shaderType, bool tessellation, bool alphatest, bool transparent)
{
	VLTYPES realVL = VLTYPE_OBJECT_POS_TEX;

	switch (shaderType)
	{
	case SHADERTYPE_TEXTURE:
		if (tessellation)
		{
			realVL = VLTYPE_OBJECT_ALL;
		}
		else
		{
			realVL = VLTYPE_OBJECT_POS_TEX;
		}
		break;
	case SHADERTYPE_DEFERRED:
	case SHADERTYPE_FORWARD:
	case SHADERTYPE_TILEDFORWARD:
		realVL = VLTYPE_OBJECT_ALL;
		break;
	case SHADERTYPE_ENVMAPCAPTURE:
		realVL = VLTYPE_OBJECT_POS_TEX;
		break;
	case SHADERTYPE_DEPTHONLY:
		if (tessellation)
		{
			realVL = VLTYPE_OBJECT_ALL;
		}
		else
		{
			if (alphatest)
			{
				realVL = VLTYPE_OBJECT_POS_TEX;
			}
			else
			{
				realVL = VLTYPE_OBJECT_POS;
			}
		}
		break;
	case SHADERTYPE_SHADOW:
	case SHADERTYPE_SHADOWCUBE:
		if (alphatest || transparent)
		{
			realVL = VLTYPE_OBJECT_POS_TEX;
		}
		else
		{
			realVL = VLTYPE_OBJECT_POS;
		}
		break;
	case SHADERTYPE_VOXELIZE:
		realVL = VLTYPE_OBJECT_POS_TEX;
		break;
	}

	return realVL;
}
VSTYPES GetVSTYPE(SHADERTYPE shaderType, bool tessellation, bool alphatest, bool transparent)
{
	VSTYPES realVS = VSTYPE_OBJECT_SIMPLE;

	switch (shaderType)
	{
	case SHADERTYPE_TEXTURE:
		if (tessellation)
		{
			realVS = VSTYPE_OBJECT_SIMPLE_TESSELLATION;
		}
		else
		{
			realVS = VSTYPE_OBJECT_SIMPLE;
		}
		break;
	case SHADERTYPE_DEFERRED:
	case SHADERTYPE_FORWARD:
	case SHADERTYPE_TILEDFORWARD:
		if (tessellation)
		{
			realVS = VSTYPE_OBJECT_COMMON_TESSELLATION;
		}
		else
		{
			realVS = VSTYPE_OBJECT_COMMON;
		}
		break;
	case SHADERTYPE_DEPTHONLY:
		if (tessellation)
		{
			realVS = VSTYPE_OBJECT_SIMPLE_TESSELLATION;
		}
		else
		{
			if (alphatest)
			{
				realVS = VSTYPE_OBJECT_SIMPLE;
			}
			else
			{
				realVS = VSTYPE_OBJECT_POSITIONSTREAM;
			}
		}
		break;
	case SHADERTYPE_ENVMAPCAPTURE:
		realVS = VSTYPE_ENVMAP;
		break;
	case SHADERTYPE_SHADOW:
		if (transparent)
		{
			realVS = VSTYPE_SHADOW_TRANSPARENT;
		}
		else
		{
			if (alphatest)
			{
				realVS = VSTYPE_SHADOW_ALPHATEST;
			}
			else
			{
				realVS = VSTYPE_SHADOW;
			}
		}
		break;
	case SHADERTYPE_SHADOWCUBE:
		if (alphatest)
		{
			realVS = VSTYPE_SHADOWCUBEMAPRENDER_ALPHATEST;
		}
		else
		{
			realVS = VSTYPE_SHADOWCUBEMAPRENDER;
		}
		break;
		break;
	case SHADERTYPE_VOXELIZE:
		realVS = VSTYPE_VOXELIZER;
		break;
	}

	return realVS;
}
GSTYPES GetGSTYPE(SHADERTYPE shaderType, bool alphatest)
{
	GSTYPES realGS = GSTYPE_NULL;

	switch (shaderType)
	{
	case SHADERTYPE_TEXTURE:
		break;
	case SHADERTYPE_DEFERRED:
		break;
	case SHADERTYPE_FORWARD:
		break;
	case SHADERTYPE_TILEDFORWARD:
		break;
	case SHADERTYPE_DEPTHONLY:
		break;
	case SHADERTYPE_ENVMAPCAPTURE:
		realGS = GSTYPE_ENVMAP;
		break;
	case SHADERTYPE_SHADOW:
		break;
	case SHADERTYPE_SHADOWCUBE:
		if (alphatest)
		{
			realGS = GSTYPE_SHADOWCUBEMAPRENDER_ALPHATEST;
		}
		else
		{
			realGS = GSTYPE_SHADOWCUBEMAPRENDER;
		}
		break;
	case SHADERTYPE_VOXELIZE:
		realGS = GSTYPE_VOXELIZER;
		break;
	}

	return realGS;
}
HSTYPES GetHSTYPE(SHADERTYPE shaderType, bool tessellation)
{
	switch (shaderType)
	{
	case SHADERTYPE_TEXTURE:
	case SHADERTYPE_DEPTHONLY:
	case SHADERTYPE_DEFERRED:
	case SHADERTYPE_FORWARD:
	case SHADERTYPE_TILEDFORWARD:
		return tessellation ? HSTYPE_OBJECT : HSTYPE_NULL;
		break;
	}

	return HSTYPE_NULL;
}
DSTYPES GetDSTYPE(SHADERTYPE shaderType, bool tessellation)
{
	switch (shaderType)
	{
	case SHADERTYPE_TEXTURE:
	case SHADERTYPE_DEPTHONLY:
	case SHADERTYPE_DEFERRED:
	case SHADERTYPE_FORWARD:
	case SHADERTYPE_TILEDFORWARD:
		return tessellation ? DSTYPE_OBJECT : DSTYPE_NULL;
		break;
	}

	return DSTYPE_NULL;
}
PSTYPES GetPSTYPE(SHADERTYPE shaderType, bool alphatest, bool transparent, bool normalmap, bool planarreflection, bool pom)
{
	PSTYPES realPS = PSTYPE_OBJECT_SIMPLEST;

	switch (shaderType)
	{
	case SHADERTYPE_DEFERRED:
		if (normalmap)
		{
			if (pom)
			{
				realPS = PSTYPE_OBJECT_DEFERRED_NORMALMAP_POM;
			}
			else
			{
				realPS = PSTYPE_OBJECT_DEFERRED_NORMALMAP;
			}
		}
		else
		{
			if (pom)
			{
				realPS = PSTYPE_OBJECT_DEFERRED_POM;
			}
			else
			{
				realPS = PSTYPE_OBJECT_DEFERRED;
			}
		}
		break;
	case SHADERTYPE_FORWARD:
		if (transparent)
		{
			if (normalmap)
			{
				if (pom)
				{
					realPS = PSTYPE_OBJECT_FORWARD_TRANSPARENT_NORMALMAP_POM;
				}
				else
				{
					realPS = PSTYPE_OBJECT_FORWARD_TRANSPARENT_NORMALMAP;
				}
				if (planarreflection)
				{
					realPS = PSTYPE_OBJECT_FORWARD_TRANSPARENT_NORMALMAP_PLANARREFLECTION;
				}
			}
			else
			{
				if (pom)
				{
					realPS = PSTYPE_OBJECT_FORWARD_TRANSPARENT_POM;
				}
				else
				{
					realPS = PSTYPE_OBJECT_FORWARD_TRANSPARENT;
				}
				if (planarreflection)
				{
					realPS = PSTYPE_OBJECT_FORWARD_TRANSPARENT_PLANARREFLECTION;
				}
			}
		}
		else
		{
			if (normalmap)
			{
				if (pom)
				{
					realPS = PSTYPE_OBJECT_FORWARD_NORMALMAP_POM;
				}
				else
				{
					realPS = PSTYPE_OBJECT_FORWARD_NORMALMAP;
				}
				if (planarreflection)
				{
					realPS = PSTYPE_OBJECT_FORWARD_NORMALMAP_PLANARREFLECTION;
				}
			}
			else
			{
				if (pom)
				{
					realPS = PSTYPE_OBJECT_FORWARD_POM;
				}
				else
				{
					realPS = PSTYPE_OBJECT_FORWARD;
				}
				if (planarreflection)
				{
					realPS = PSTYPE_OBJECT_FORWARD_PLANARREFLECTION;
				}
			}
		}
		break;
	case SHADERTYPE_TILEDFORWARD:
		if (transparent)
		{
			if (normalmap)
			{
				if (pom)
				{
					realPS = PSTYPE_OBJECT_TILEDFORWARD_TRANSPARENT_NORMALMAP_POM;
				}
				else
				{
					realPS = PSTYPE_OBJECT_TILEDFORWARD_TRANSPARENT_NORMALMAP;
				}
				if (planarreflection)
				{
					realPS = PSTYPE_OBJECT_TILEDFORWARD_TRANSPARENT_NORMALMAP_PLANARREFLECTION;
				}
			}
			else
			{
				if (pom)
				{
					realPS = PSTYPE_OBJECT_TILEDFORWARD_TRANSPARENT_POM;
				}
				else
				{
					realPS = PSTYPE_OBJECT_TILEDFORWARD_TRANSPARENT;
				}
				if (planarreflection)
				{
					realPS = PSTYPE_OBJECT_TILEDFORWARD_TRANSPARENT_PLANARREFLECTION;
				}
			}
		}
		else
		{
			if (normalmap)
			{
				if (pom)
				{
					realPS = PSTYPE_OBJECT_TILEDFORWARD_NORMALMAP_POM;
				}
				else
				{
					realPS = PSTYPE_OBJECT_TILEDFORWARD_NORMALMAP;
				}
				if (planarreflection)
				{
					realPS = PSTYPE_OBJECT_TILEDFORWARD_NORMALMAP_PLANARREFLECTION;
				}
			}
			else
			{
				if (pom)
				{
					realPS = PSTYPE_OBJECT_TILEDFORWARD_POM;
				}
				else
				{
					realPS = PSTYPE_OBJECT_TILEDFORWARD;
				}
				if (planarreflection)
				{
					realPS = PSTYPE_OBJECT_TILEDFORWARD_PLANARREFLECTION;
				}
			}
		}
		break;
	case SHADERTYPE_SHADOW:
		if (transparent)
		{
			realPS = PSTYPE_SHADOW_TRANSPARENT;
		}
		else
		{
			if (alphatest)
			{
				realPS = PSTYPE_SHADOW_ALPHATEST;
			}
			else
			{
				realPS = PSTYPE_NULL;
			}
		}
		break;
	case SHADERTYPE_SHADOWCUBE:
		if (alphatest)
		{
			realPS = PSTYPE_SHADOWCUBEMAPRENDER_ALPHATEST;
		}
		else
		{
			realPS = PSTYPE_SHADOWCUBEMAPRENDER;
		}
		break;
	case SHADERTYPE_ENVMAPCAPTURE:
		realPS = PSTYPE_ENVMAP;
		break;
	case SHADERTYPE_DEPTHONLY:
		if (alphatest)
		{
			realPS = PSTYPE_OBJECT_ALPHATESTONLY;
		}
		else
		{
			realPS = PSTYPE_NULL;
		}
		break;
	case SHADERTYPE_VOXELIZE:
		realPS = PSTYPE_VOXELIZER;
		break;
	case SHADERTYPE_TEXTURE:
		realPS = PSTYPE_OBJECT_TEXTUREONLY;
		break;
	}

	return realPS;
}

GraphicsPSO* PSO_decal = nullptr;
GraphicsPSO* PSO_occlusionquery = nullptr;
GraphicsPSO* PSO_impostor[SHADERTYPE_COUNT] = {};
GraphicsPSO* PSO_captureimpostor = nullptr;
GraphicsPSO* GetImpostorPSO(SHADERTYPE shaderType)
{
	if (wiRenderer::IsWireRender())
	{
		switch (shaderType)
		{
		case SHADERTYPE_TEXTURE:
		case SHADERTYPE_DEFERRED:
		case SHADERTYPE_FORWARD:
		case SHADERTYPE_TILEDFORWARD:
			return PSO_object_wire;
		}
		return nullptr;
	}

	return PSO_impostor[shaderType];
}

GraphicsPSO* PSO_deferredlight[LightComponent::LIGHTTYPE_COUNT] = {};
GraphicsPSO* PSO_lightvisualizer[LightComponent::LIGHTTYPE_COUNT] = {};
GraphicsPSO* PSO_volumetriclight[LightComponent::LIGHTTYPE_COUNT] = {};
GraphicsPSO* PSO_enviromentallight = nullptr;

enum SKYRENDERING
{
	SKYRENDERING_STATIC,
	SKYRENDERING_DYNAMIC,
	SKYRENDERING_SUN,
	SKYRENDERING_ENVMAPCAPTURE_STATIC,
	SKYRENDERING_ENVMAPCAPTURE_DYNAMIC,
	SKYRENDERING_COUNT
};
GraphicsPSO* PSO_sky[SKYRENDERING_COUNT] = {};

enum DEBUGRENDERING
{
	DEBUGRENDERING_ENVPROBE,
	DEBUGRENDERING_GRID,
	DEBUGRENDERING_CUBE,
	DEBUGRENDERING_LINES,
	DEBUGRENDERING_EMITTER,
	DEBUGRENDERING_VOXEL,
	DEBUGRENDERING_FORCEFIELD_POINT,
	DEBUGRENDERING_FORCEFIELD_PLANE,
	DEBUGRENDERING_COUNT
};
GraphicsPSO* PSO_debug[DEBUGRENDERING_COUNT] = {};

enum TILEDLIGHTING_TYPE
{
	TILEDLIGHTING_TYPE_FORWARD,
	TILEDLIGHTING_TYPE_DEFERRED,
	TILEDLIGHTING_TYPE_COUNT
};
enum TILEDLIGHTING_CULLING
{
	TILEDLIGHTING_CULLING_BASIC,
	TILEDLIGHTING_CULLING_ADVANCED,
	TILEDLIGHTING_CULLING_COUNT
};
enum TILEDLIGHTING_DEBUG
{
	TILEDLIGHTING_DEBUG_DISABLED,
	TILEDLIGHTING_DEBUG_ENABLED,
	TILEDLIGHTING_DEBUG_COUNT
};
ComputePSO* CPSO_tiledlighting[TILEDLIGHTING_TYPE_COUNT][TILEDLIGHTING_CULLING_COUNT][TILEDLIGHTING_DEBUG_COUNT] = {};
ComputePSO* CPSO[CSTYPE_LAST] = {};

void wiRenderer::LoadShaders()
{
	GraphicsDevice* device = GetDevice();

	for (int i = 0; i < VLTYPE_LAST; ++i)
	{
		vertexLayouts[i] = new VertexLayout;
	}

	{
		VertexLayoutDesc layout[] =
		{
			{ "POSITION_NORMAL_SUBSETINDEX",	0, MeshComponent::Vertex_POS::FORMAT, 0, APPEND_ALIGNED_ELEMENT, INPUT_PER_VERTEX_DATA, 0 },
		};
		vertexShaders[VSTYPE_OBJECT_DEBUG] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectVS_debug.cso", wiResourceManager::VERTEXSHADER));
		device->CreateInputLayout(layout, ARRAYSIZE(layout), vertexShaders[VSTYPE_OBJECT_DEBUG]->code.data, vertexShaders[VSTYPE_OBJECT_DEBUG]->code.size, vertexLayouts[VLTYPE_OBJECT_DEBUG]);
	}
	{
		VertexLayoutDesc layout[] =
		{
			{ "POSITION_NORMAL_SUBSETINDEX",	0, MeshComponent::Vertex_POS::FORMAT, 0, APPEND_ALIGNED_ELEMENT, INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD",				0, MeshComponent::Vertex_TEX::FORMAT, 1, APPEND_ALIGNED_ELEMENT, INPUT_PER_VERTEX_DATA, 0 },
			{ "PREVPOS",				0, MeshComponent::Vertex_POS::FORMAT, 2, APPEND_ALIGNED_ELEMENT, INPUT_PER_VERTEX_DATA, 0 },

			{ "MATI",			0, FORMAT_R32G32B32A32_FLOAT, 3, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "MATI",			1, FORMAT_R32G32B32A32_FLOAT, 3, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "MATI",			2, FORMAT_R32G32B32A32_FLOAT, 3, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "COLOR_DITHER",	0, FORMAT_R32G32B32A32_FLOAT, 3, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "MATIPREV",		0, FORMAT_R32G32B32A32_FLOAT, 3, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "MATIPREV",		1, FORMAT_R32G32B32A32_FLOAT, 3, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "MATIPREV",		2, FORMAT_R32G32B32A32_FLOAT, 3, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
		};
		vertexShaders[VSTYPE_OBJECT_COMMON] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectVS_common.cso", wiResourceManager::VERTEXSHADER));
		device->CreateInputLayout(layout, ARRAYSIZE(layout), vertexShaders[VSTYPE_OBJECT_COMMON]->code.data, vertexShaders[VSTYPE_OBJECT_COMMON]->code.size, vertexLayouts[VLTYPE_OBJECT_ALL]);
		
	}
	{
		VertexLayoutDesc layout[] =
		{
			{ "POSITION_NORMAL_SUBSETINDEX",	0, MeshComponent::Vertex_POS::FORMAT, 0, APPEND_ALIGNED_ELEMENT, INPUT_PER_VERTEX_DATA, 0 },

			{ "MATI",			0, FORMAT_R32G32B32A32_FLOAT, 1, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "MATI",			1, FORMAT_R32G32B32A32_FLOAT, 1, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "MATI",			2, FORMAT_R32G32B32A32_FLOAT, 1, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "COLOR_DITHER",	0, FORMAT_R32G32B32A32_FLOAT, 1, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
		};
		vertexShaders[VSTYPE_OBJECT_POSITIONSTREAM] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectVS_positionstream.cso", wiResourceManager::VERTEXSHADER));
		device->CreateInputLayout(layout, ARRAYSIZE(layout), vertexShaders[VSTYPE_OBJECT_POSITIONSTREAM]->code.data, vertexShaders[VSTYPE_OBJECT_POSITIONSTREAM]->code.size, vertexLayouts[VLTYPE_OBJECT_POS]);

	}
	{
		VertexLayoutDesc layout[] =
		{
			{ "POSITION_NORMAL_SUBSETINDEX",	0, MeshComponent::Vertex_POS::FORMAT, 0, APPEND_ALIGNED_ELEMENT, INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD",				0, MeshComponent::Vertex_TEX::FORMAT, 1, APPEND_ALIGNED_ELEMENT, INPUT_PER_VERTEX_DATA, 0 },

			{ "MATI",			0, FORMAT_R32G32B32A32_FLOAT, 2, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "MATI",			1, FORMAT_R32G32B32A32_FLOAT, 2, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "MATI",			2, FORMAT_R32G32B32A32_FLOAT, 2, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "COLOR_DITHER",	0, FORMAT_R32G32B32A32_FLOAT, 2, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
		};
		vertexShaders[VSTYPE_OBJECT_SIMPLE] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectVS_simple.cso", wiResourceManager::VERTEXSHADER));
		device->CreateInputLayout(layout, ARRAYSIZE(layout), vertexShaders[VSTYPE_OBJECT_SIMPLE]->code.data, vertexShaders[VSTYPE_OBJECT_SIMPLE]->code.size, vertexLayouts[VLTYPE_OBJECT_POS_TEX]);

	}
	{
		VertexLayoutDesc layout[] =
		{
			{ "POSITION_NORMAL_SUBSETINDEX",	0, MeshComponent::Vertex_POS::FORMAT, 0, APPEND_ALIGNED_ELEMENT, INPUT_PER_VERTEX_DATA, 0 },

			{ "MATI",			0, FORMAT_R32G32B32A32_FLOAT, 1, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "MATI",			1, FORMAT_R32G32B32A32_FLOAT, 1, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "MATI",			2, FORMAT_R32G32B32A32_FLOAT, 1, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "COLOR_DITHER",	0, FORMAT_R32G32B32A32_FLOAT, 1, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
		};
		vertexShaders[VSTYPE_SHADOW] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "shadowVS.cso", wiResourceManager::VERTEXSHADER));
		device->CreateInputLayout(layout, ARRAYSIZE(layout), vertexShaders[VSTYPE_SHADOW]->code.data, vertexShaders[VSTYPE_SHADOW]->code.size, vertexLayouts[VLTYPE_SHADOW_POS]);

	}
	{
		VertexLayoutDesc layout[] =
		{
			{ "POSITION_NORMAL_SUBSETINDEX",	0, MeshComponent::Vertex_POS::FORMAT, 0, APPEND_ALIGNED_ELEMENT, INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD",				0, MeshComponent::Vertex_TEX::FORMAT, 1, APPEND_ALIGNED_ELEMENT, INPUT_PER_VERTEX_DATA, 0 },

			{ "MATI",			0, FORMAT_R32G32B32A32_FLOAT, 2, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "MATI",			1, FORMAT_R32G32B32A32_FLOAT, 2, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "MATI",			2, FORMAT_R32G32B32A32_FLOAT, 2, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
			{ "COLOR_DITHER",	0, FORMAT_R32G32B32A32_FLOAT, 2, APPEND_ALIGNED_ELEMENT, INPUT_PER_INSTANCE_DATA, 1 },
		};
		vertexShaders[VSTYPE_SHADOW_ALPHATEST] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "shadowVS_alphatest.cso", wiResourceManager::VERTEXSHADER));
		device->CreateInputLayout(layout, ARRAYSIZE(layout), vertexShaders[VSTYPE_SHADOW_ALPHATEST]->code.data, vertexShaders[VSTYPE_SHADOW_ALPHATEST]->code.size, vertexLayouts[VLTYPE_SHADOW_POS_TEX]);


		vertexShaders[VSTYPE_SHADOW_TRANSPARENT] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "shadowVS_transparent.cso", wiResourceManager::VERTEXSHADER));

	}

	{
		VertexLayoutDesc layout[] =
		{
			{ "POSITION", 0, FORMAT_R32G32B32A32_FLOAT, 0, APPEND_ALIGNED_ELEMENT, INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, FORMAT_R32G32B32A32_FLOAT, 0, APPEND_ALIGNED_ELEMENT, INPUT_PER_VERTEX_DATA, 0 },
		};
		vertexShaders[VSTYPE_LINE] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "linesVS.cso", wiResourceManager::VERTEXSHADER));
		device->CreateInputLayout(layout, ARRAYSIZE(layout), vertexShaders[VSTYPE_LINE]->code.data, vertexShaders[VSTYPE_LINE]->code.size, vertexLayouts[VLTYPE_LINE]);

	}

	{
		VertexLayoutDesc layout[] =
		{
			{ "POSITION", 0, FORMAT_R32G32B32_FLOAT, 0, APPEND_ALIGNED_ELEMENT, INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, FORMAT_R32G32_FLOAT, 0, APPEND_ALIGNED_ELEMENT, INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 1, FORMAT_R32G32B32A32_FLOAT, 0, APPEND_ALIGNED_ELEMENT, INPUT_PER_VERTEX_DATA, 0 },
		};
		vertexShaders[VSTYPE_TRAIL] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "trailVS.cso", wiResourceManager::VERTEXSHADER));
		device->CreateInputLayout(layout, ARRAYSIZE(layout), vertexShaders[VSTYPE_TRAIL]->code.data, vertexShaders[VSTYPE_TRAIL]->code.size, vertexLayouts[VLTYPE_TRAIL]);

	}

	vertexShaders[VSTYPE_OBJECT_COMMON_TESSELLATION] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectVS_common_tessellation.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_OBJECT_SIMPLE_TESSELLATION] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectVS_simple_tessellation.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_DIRLIGHT] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "dirLightVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_POINTLIGHT] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "pointLightVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_SPOTLIGHT] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "spotLightVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_LIGHTVISUALIZER_SPOTLIGHT] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "vSpotLightVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_LIGHTVISUALIZER_POINTLIGHT] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "vPointLightVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_LIGHTVISUALIZER_SPHERELIGHT] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "vSphereLightVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_LIGHTVISUALIZER_DISCLIGHT] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "vDiscLightVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_LIGHTVISUALIZER_RECTANGLELIGHT] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "vRectangleLightVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_LIGHTVISUALIZER_TUBELIGHT] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "vTubeLightVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_DECAL] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "decalVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_ENVMAP] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "envMapVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_ENVMAP_SKY] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "envMap_skyVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_SPHERE] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "sphereVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_CUBE] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "cubeVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_SHADOWCUBEMAPRENDER] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "cubeShadowVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_SHADOWCUBEMAPRENDER_ALPHATEST] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "cubeShadowVS_alphatest.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_SKY] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "skyVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_WATER] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "waterVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_VOXELIZER] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectVS_voxelizer.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_VOXEL] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "voxelVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_FORCEFIELDVISUALIZER_POINT] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "forceFieldPointVisualizerVS.cso", wiResourceManager::VERTEXSHADER));
	vertexShaders[VSTYPE_FORCEFIELDVISUALIZER_PLANE] = static_cast<VertexShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "forceFieldPlaneVisualizerVS.cso", wiResourceManager::VERTEXSHADER));


	pixelShaders[PSTYPE_OBJECT_DEFERRED] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_deferred.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_DEFERRED_NORMALMAP] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_deferred_normalmap.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_DEFERRED_POM] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_deferred_pom.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_DEFERRED_NORMALMAP_POM] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_deferred_normalmap_pom.cso", wiResourceManager::PIXELSHADER));

	pixelShaders[PSTYPE_OBJECT_FORWARD] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_forward.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_FORWARD_NORMALMAP] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_forward_normalmap.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_FORWARD_TRANSPARENT] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_forward_transparent.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_FORWARD_TRANSPARENT_NORMALMAP] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_forward_transparent_normalmap.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_FORWARD_PLANARREFLECTION] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_forward_planarreflection.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_FORWARD_NORMALMAP_PLANARREFLECTION] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_forward_normalmap_planarreflection.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_FORWARD_TRANSPARENT_PLANARREFLECTION] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_forward_transparent_planarreflection.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_FORWARD_TRANSPARENT_NORMALMAP_PLANARREFLECTION] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_forward_transparent_normalmap_planarreflection.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_FORWARD_POM] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_forward_pom.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_FORWARD_NORMALMAP_POM] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_forward_normalmap_pom.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_FORWARD_TRANSPARENT_POM] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_forward_transparent_pom.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_FORWARD_TRANSPARENT_NORMALMAP_POM] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_forward_transparent_normalmap_pom.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_FORWARD_WATER] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_forward_water.cso", wiResourceManager::PIXELSHADER));

	pixelShaders[PSTYPE_OBJECT_TILEDFORWARD] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_tiledforward.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_TILEDFORWARD_NORMALMAP] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_tiledforward_normalmap.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_TILEDFORWARD_TRANSPARENT] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_tiledforward_transparent.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_TILEDFORWARD_TRANSPARENT_NORMALMAP] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_tiledforward_transparent_normalmap.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_TILEDFORWARD_PLANARREFLECTION] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_tiledforward_planarreflection.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_TILEDFORWARD_NORMALMAP_PLANARREFLECTION] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_tiledforward_normalmap_planarreflection.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_TILEDFORWARD_TRANSPARENT_PLANARREFLECTION] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_tiledforward_transparent_planarreflection.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_TILEDFORWARD_TRANSPARENT_NORMALMAP_PLANARREFLECTION] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_tiledforward_transparent_normalmap_planarreflection.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_TILEDFORWARD_POM] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_tiledforward_pom.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_TILEDFORWARD_NORMALMAP_POM] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_tiledforward_normalmap_pom.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_TILEDFORWARD_TRANSPARENT_POM] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_tiledforward_transparent_pom.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_TILEDFORWARD_TRANSPARENT_NORMALMAP_POM] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_tiledforward_transparent_normalmap_pom.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_TILEDFORWARD_WATER] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_tiledforward_water.cso", wiResourceManager::PIXELSHADER));

	pixelShaders[PSTYPE_OBJECT_HOLOGRAM] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_hologram.cso", wiResourceManager::PIXELSHADER));


	pixelShaders[PSTYPE_OBJECT_DEBUG] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_debug.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_SIMPLEST] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_simplest.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_BLACKOUT] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_blackout.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_TEXTUREONLY] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_textureonly.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_OBJECT_ALPHATESTONLY] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_alphatestonly.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_ENVIRONMENTALLIGHT] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "environmentalLightPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_DIRLIGHT] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "dirLightPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_POINTLIGHT] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "pointLightPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_SPOTLIGHT] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "spotLightPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_SPHERELIGHT] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "sphereLightPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_DISCLIGHT] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "discLightPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_RECTANGLELIGHT] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "rectangleLightPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_TUBELIGHT] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "tubeLightPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_LIGHTVISUALIZER] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "lightVisualizerPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_VOLUMETRICLIGHT_DIRECTIONAL] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "volumetricLight_DirectionalPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_VOLUMETRICLIGHT_POINT] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "volumetricLight_PointPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_VOLUMETRICLIGHT_SPOT] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "volumetricLight_SpotPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_DECAL] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "decalPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_ENVMAP] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "envMapPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_ENVMAP_SKY_STATIC] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "envMap_skyPS_static.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_ENVMAP_SKY_DYNAMIC] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "envMap_skyPS_dynamic.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_CAPTUREIMPOSTOR] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "captureImpostorPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_CUBEMAP] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "cubemapPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_LINE] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "linesPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_SKY_STATIC] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "skyPS_static.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_SKY_DYNAMIC] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "skyPS_dynamic.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_SUN] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "sunPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_SHADOW_ALPHATEST] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "shadowPS_alphatest.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_SHADOW_TRANSPARENT] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "shadowPS_transparent.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_SHADOW_WATER] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "shadowPS_water.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_SHADOWCUBEMAPRENDER] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "cubeShadowPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_SHADOWCUBEMAPRENDER_ALPHATEST] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "cubeShadowPS_alphatest.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_TRAIL] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "trailPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_VOXELIZER] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectPS_voxelizer.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_VOXEL] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "voxelPS.cso", wiResourceManager::PIXELSHADER));
	pixelShaders[PSTYPE_FORCEFIELDVISUALIZER] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "forceFieldVisualizerPS.cso", wiResourceManager::PIXELSHADER));

	geometryShaders[GSTYPE_ENVMAP] = static_cast<GeometryShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "envMapGS.cso", wiResourceManager::GEOMETRYSHADER));
	geometryShaders[GSTYPE_ENVMAP_SKY] = static_cast<GeometryShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "envMap_skyGS.cso", wiResourceManager::GEOMETRYSHADER));
	geometryShaders[GSTYPE_SHADOWCUBEMAPRENDER] = static_cast<GeometryShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "cubeShadowGS.cso", wiResourceManager::GEOMETRYSHADER));
	geometryShaders[GSTYPE_SHADOWCUBEMAPRENDER_ALPHATEST] = static_cast<GeometryShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "cubeShadowGS_alphatest.cso", wiResourceManager::GEOMETRYSHADER));
	geometryShaders[GSTYPE_VOXELIZER] = static_cast<GeometryShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectGS_voxelizer.cso", wiResourceManager::GEOMETRYSHADER));
	geometryShaders[GSTYPE_VOXEL] = static_cast<GeometryShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "voxelGS.cso", wiResourceManager::GEOMETRYSHADER));


	computeShaders[CSTYPE_LUMINANCE_PASS1] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "luminancePass1CS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_LUMINANCE_PASS2] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "luminancePass2CS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_TILEFRUSTUMS] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "tileFrustumsCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_RESOLVEMSAADEPTHSTENCIL] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "resolveMSAADepthStencilCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_VOXELSCENECOPYCLEAR] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "voxelSceneCopyClearCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_VOXELSCENECOPYCLEAR_TEMPORALSMOOTHING] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "voxelSceneCopyClear_TemporalSmoothing.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_VOXELRADIANCESECONDARYBOUNCE] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "voxelRadianceSecondaryBounceCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_VOXELCLEARONLYNORMAL] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "voxelClearOnlyNormalCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_GENERATEMIPCHAIN2D_UNORM4_SIMPLEFILTER] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "generateMIPChain2D_unorm4_SimpleFilterCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_GENERATEMIPCHAIN2D_FLOAT4_SIMPLEFILTER] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "generateMIPChain2D_float4_SimpleFilterCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_GENERATEMIPCHAIN2D_UNORM4_GAUSSIAN] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "generateMIPChain2D_unorm4_GaussianCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_GENERATEMIPCHAIN2D_FLOAT4_GAUSSIAN] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "generateMIPChain2D_float4_GaussianCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_GENERATEMIPCHAIN3D_UNORM4_SIMPLEFILTER] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "generateMIPChain3D_unorm4_SimpleFilterCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_GENERATEMIPCHAIN3D_FLOAT4_SIMPLEFILTER] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "generateMIPChain3D_float4_SimpleFilterCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_GENERATEMIPCHAIN3D_UNORM4_GAUSSIAN] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "generateMIPChain3D_unorm4_GaussianCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_GENERATEMIPCHAIN3D_FLOAT4_GAUSSIAN] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "generateMIPChain3D_float4_GaussianCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_GENERATEMIPCHAINCUBE_UNORM4_SIMPLEFILTER] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "generateMIPChainCube_unorm4_SimpleFilterCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_GENERATEMIPCHAINCUBE_FLOAT4_SIMPLEFILTER] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "generateMIPChainCube_float4_SimpleFilterCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_GENERATEMIPCHAINCUBEARRAY_UNORM4_SIMPLEFILTER] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "generateMIPChainCubeArray_unorm4_SimpleFilterCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_GENERATEMIPCHAINCUBEARRAY_FLOAT4_SIMPLEFILTER] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "generateMIPChainCubeArray_float4_SimpleFilterCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_FILTERENVMAP] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "filterEnvMapCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_COPYTEXTURE2D_UNORM4] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "copytexture2D_unorm4CS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_COPYTEXTURE2D_UNORM4_BORDEREXPAND] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "copytexture2D_unorm4_borderexpandCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_SKINNING] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "skinningCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_SKINNING_LDS] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "skinningCS_LDS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_CLOUDGENERATOR] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "cloudGeneratorCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_BVH_RESET] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "bvh_resetCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_BVH_CLASSIFICATION] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "bvh_classificationCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_BVH_KICKJOBS] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "bvh_kickjobsCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_BVH_CLUSTERPROCESSOR] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "bvh_clusterprocessorCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_BVH_HIERARCHY] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "bvh_hierarchyCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_BVH_PROPAGATEAABB] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "bvh_propagateaabbCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_RAYTRACE_CLEAR] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "raytrace_clearCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_RAYTRACE_LAUNCH] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "raytrace_launchCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_RAYTRACE_KICKJOBS] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "raytrace_kickjobsCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_RAYTRACE_PRIMARY] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "raytrace_primaryCS.cso", wiResourceManager::COMPUTESHADER));
	computeShaders[CSTYPE_RAYTRACE_LIGHTSAMPLING] = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "raytrace_lightsamplingCS.cso", wiResourceManager::COMPUTESHADER));


	hullShaders[HSTYPE_OBJECT] = static_cast<HullShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectHS.cso", wiResourceManager::HULLSHADER));


	domainShaders[DSTYPE_OBJECT] = static_cast<DomainShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + "objectDS.cso", wiResourceManager::DOMAINSHADER));


	vector<thread> thread_pool(0);

	thread_pool.push_back(thread([&] {
		// default objectshaders:
		for (int shaderType = 0; shaderType < SHADERTYPE_COUNT; ++shaderType)
		{
			for (int doublesided = 0; doublesided < OBJECTRENDERING_DOUBLESIDED_COUNT; ++doublesided)
			{
				for (int tessellation = 0; tessellation < OBJECTRENDERING_TESSELLATION_COUNT; ++tessellation)
				{
					for (int alphatest = 0; alphatest < OBJECTRENDERING_ALPHATEST_COUNT; ++alphatest)
					{
						for (int transparency = 0; transparency < OBJECTRENDERING_TRANSPARENCY_COUNT; ++transparency)
						{
							for (int normalmap = 0; normalmap < OBJECTRENDERING_NORMALMAP_COUNT; ++normalmap)
							{
								for (int planarreflection = 0; planarreflection < OBJECTRENDERING_PLANARREFLECTION_COUNT; ++planarreflection)
								{
									for (int pom = 0; pom < OBJECTRENDERING_POM_COUNT; ++pom)
									{
										VSTYPES realVS = GetVSTYPE((SHADERTYPE)shaderType, tessellation, alphatest, transparency);
										VLTYPES realVL = GetVLTYPE((SHADERTYPE)shaderType, tessellation, alphatest, transparency);
										HSTYPES realHS = GetHSTYPE((SHADERTYPE)shaderType, tessellation);
										DSTYPES realDS = GetDSTYPE((SHADERTYPE)shaderType, tessellation);
										GSTYPES realGS = GetGSTYPE((SHADERTYPE)shaderType, alphatest);
										PSTYPES realPS = GetPSTYPE((SHADERTYPE)shaderType, alphatest, transparency, normalmap, planarreflection, pom);

										if (tessellation && (realHS == HSTYPE_NULL || realDS == DSTYPE_NULL))
										{
											continue;
										}

										GraphicsPSODesc desc;
										desc.vs = vertexShaders[realVS];
										desc.il = vertexLayouts[realVL];
										desc.hs = hullShaders[realHS];
										desc.ds = domainShaders[realDS];
										desc.gs = geometryShaders[realGS];
										desc.ps = pixelShaders[realPS];

										switch (shaderType)
										{
										case SHADERTYPE_DEPTHONLY:
										case SHADERTYPE_SHADOW:
										case SHADERTYPE_SHADOWCUBE:
											desc.bs = blendStates[transparency ? BSTYPE_TRANSPARENTSHADOWMAP : BSTYPE_COLORWRITEDISABLE];
											break;
										default:
											desc.bs = blendStates[transparency ? BSTYPE_TRANSPARENT : BSTYPE_OPAQUE];
											break;
										}

										switch (shaderType)
										{
										case SHADERTYPE_SHADOW:
										case SHADERTYPE_SHADOWCUBE:
											desc.dss = depthStencils[transparency ? DSSTYPE_DEPTHREAD : DSSTYPE_SHADOW];
											break;
										case SHADERTYPE_TILEDFORWARD:
											desc.dss = depthStencils[transparency ? DSSTYPE_DEFAULT : DSSTYPE_DEPTHREADEQUAL];
											break;
										case SHADERTYPE_ENVMAPCAPTURE:
											desc.dss = depthStencils[DSSTYPE_ENVMAP];
											break;
										case SHADERTYPE_VOXELIZE:
											desc.dss = depthStencils[DSSTYPE_XRAY];
											break;
										default:
											desc.dss = depthStencils[DSSTYPE_DEFAULT];
											break;
										}

										switch (shaderType)
										{
										case SHADERTYPE_SHADOW:
										case SHADERTYPE_SHADOWCUBE:
											desc.rs = rasterizers[doublesided ? RSTYPE_SHADOW_DOUBLESIDED : RSTYPE_SHADOW];
											break;
										case SHADERTYPE_VOXELIZE:
											desc.rs = rasterizers[RSTYPE_VOXELIZE];
											break;
										default:
											desc.rs = rasterizers[doublesided ? RSTYPE_DOUBLESIDED : RSTYPE_FRONT];
											break;
										}

										switch (shaderType)
										{
										case SHADERTYPE_TEXTURE:
											desc.numRTs = 1;
											desc.RTFormats[0] = RTFormat_hdr;
											desc.DSFormat = DSFormat_full;
											break;
										case SHADERTYPE_DEFERRED:
											desc.numRTs = 4;
											desc.RTFormats[0] = RTFormat_gbuffer_0;
											desc.RTFormats[1] = RTFormat_gbuffer_1;
											desc.RTFormats[2] = RTFormat_gbuffer_2;
											desc.RTFormats[3] = RTFormat_gbuffer_3;
											desc.DSFormat = DSFormat_full;
											break;
										case SHADERTYPE_FORWARD:
											if (transparency)
											{
												desc.numRTs = 1;
											}
											else
											{
												desc.numRTs = 2;
											}
											desc.RTFormats[0] = RTFormat_hdr;
											desc.RTFormats[1] = RTFormat_gbuffer_1;
											desc.DSFormat = DSFormat_full;
											break;
										case SHADERTYPE_TILEDFORWARD:
											if (transparency)
											{
												desc.numRTs = 1;
											}
											else
											{
												desc.numRTs = 2;
											}
											desc.RTFormats[0] = RTFormat_hdr;
											desc.RTFormats[1] = RTFormat_gbuffer_1;
											desc.DSFormat = DSFormat_full;
											break;
										case SHADERTYPE_DEPTHONLY:
											desc.numRTs = 0;
											desc.DSFormat = DSFormat_full;
											break;
										case SHADERTYPE_ENVMAPCAPTURE:
											desc.numRTs = 1;
											desc.RTFormats[0] = RTFormat_envprobe;
											desc.DSFormat = DSFormat_small;
											break;
										case SHADERTYPE_SHADOW:
											if (transparency)
											{
												desc.numRTs = 1;
												desc.RTFormats[0] = RTFormat_ldr;
											}
											else
											{
												desc.numRTs = 0;
											}
											desc.DSFormat = DSFormat_small;
											break;
										case SHADERTYPE_SHADOWCUBE:
											desc.numRTs = 0;
											desc.DSFormat = DSFormat_small;
											break;
										case SHADERTYPE_VOXELIZE:
											desc.numRTs = 0;
											break;
										}

										if (tessellation)
										{
											desc.pt = PATCHLIST;
										}
										else
										{
											desc.pt = TRIANGLELIST;
										}

										RECREATE(PSO_object[shaderType][doublesided][tessellation][alphatest][transparency][normalmap][planarreflection][pom]);
										device->CreateGraphicsPSO(&desc, PSO_object[shaderType][doublesided][tessellation][alphatest][transparency][normalmap][planarreflection][pom]);
									}
								}
							}
						}
					}
				}
			}
		}

		//// Custom objectshader presets:
		//for (auto& x : Material::customShaderPresets)
		//{
		//	SAFE_DELETE(x);
		//}
		//Material::customShaderPresets.clear();

		//// Hologram:
		//{
		//	VSTYPES realVS = GetVSTYPE(SHADERTYPE_FORWARD, false, false, true);
		//	VLTYPES realVL = GetVLTYPE(SHADERTYPE_FORWARD, false, false, true);

		//	GraphicsPSODesc desc;
		//	desc.vs = vertexShaders[realVS];
		//	desc.il = vertexLayouts[realVL];
		//	desc.ps = pixelShaders[PSTYPE_OBJECT_HOLOGRAM];

		//	desc.bs = blendStates[BSTYPE_ADDITIVE];
		//	desc.rs = rasterizers[DSSTYPE_DEFAULT];
		//	desc.dss = depthStencils[DSSTYPE_DEPTHREAD];
		//	desc.pt = TRIANGLELIST;

		//	desc.numRTs = 1;
		//	desc.RTFormats[0] = RTFormat_hdr;
		//	desc.DSFormat = DSFormat_full;

		//	Material::CustomShader* customShader = new Material::CustomShader;
		//	customShader->name = "Hologram";
		//	customShader->passes[SHADERTYPE_FORWARD].pso = new GraphicsPSO;
		//	device->CreateGraphicsPSO(&desc, customShader->passes[SHADERTYPE_FORWARD].pso);
		//	customShader->passes[SHADERTYPE_TILEDFORWARD].pso = new GraphicsPSO;
		//	device->CreateGraphicsPSO(&desc, customShader->passes[SHADERTYPE_TILEDFORWARD].pso);
		//	Material::customShaderPresets.push_back(customShader);
		//}


	}));

	thread_pool.push_back(thread([&] {
		{
			GraphicsPSODesc desc;
			desc.vs = vertexShaders[VSTYPE_WATER];
			desc.rs = rasterizers[RSTYPE_DOUBLESIDED];
			desc.bs = blendStates[BSTYPE_TRANSPARENT];
			desc.dss = depthStencils[DSSTYPE_DEFAULT];
			desc.il = vertexLayouts[VLTYPE_OBJECT_POS_TEX];

			desc.numRTs = 1;
			desc.RTFormats[0] = RTFormat_hdr;
			desc.DSFormat = DSFormat_full;

			desc.ps = pixelShaders[PSTYPE_OBJECT_FORWARD_WATER];
			RECREATE(PSO_object_water[SHADERTYPE_FORWARD]);
			device->CreateGraphicsPSO(&desc, PSO_object_water[SHADERTYPE_FORWARD]);

			desc.ps = pixelShaders[PSTYPE_OBJECT_TILEDFORWARD_WATER];
			RECREATE(PSO_object_water[SHADERTYPE_TILEDFORWARD]);
			device->CreateGraphicsPSO(&desc, PSO_object_water[SHADERTYPE_TILEDFORWARD]);

			desc.dss = depthStencils[DSSTYPE_DEPTHREAD];
			desc.rs = rasterizers[RSTYPE_SHADOW];
			desc.bs = blendStates[BSTYPE_TRANSPARENTSHADOWMAP];
			desc.vs = vertexShaders[VSTYPE_SHADOW_TRANSPARENT];
			desc.ps = pixelShaders[PSTYPE_SHADOW_WATER];
			RECREATE(PSO_object_water[SHADERTYPE_SHADOW]);
			device->CreateGraphicsPSO(&desc, PSO_object_water[SHADERTYPE_SHADOW]);
		}
		{
			GraphicsPSODesc desc;
			desc.vs = vertexShaders[VSTYPE_OBJECT_SIMPLE];
			desc.ps = pixelShaders[PSTYPE_OBJECT_SIMPLEST];
			desc.rs = rasterizers[RSTYPE_WIRE];
			desc.bs = blendStates[BSTYPE_OPAQUE];
			desc.dss = depthStencils[DSSTYPE_DEFAULT];
			desc.il = vertexLayouts[VLTYPE_OBJECT_POS_TEX];

			desc.numRTs = 1;
			desc.RTFormats[0] = RTFormat_hdr;
			desc.DSFormat = DSFormat_full;

			RECREATE(PSO_object_wire);
			device->CreateGraphicsPSO(&desc, PSO_object_wire);
		}
		{
			GraphicsPSODesc desc;
			desc.vs = vertexShaders[VSTYPE_DECAL];
			desc.ps = pixelShaders[PSTYPE_DECAL];
			desc.rs = rasterizers[RSTYPE_FRONT];
			desc.bs = blendStates[BSTYPE_DECAL];
			desc.dss = depthStencils[DSSTYPE_DECAL];
			desc.pt = TRIANGLESTRIP;

			desc.numRTs = 1;
			desc.RTFormats[0] = RTFormat_gbuffer_0;
			//desc.RTFormats[1] = RTFormat_gbuffer_1;

			RECREATE(PSO_decal);
			device->CreateGraphicsPSO(&desc, PSO_decal);
		}
		{
			GraphicsPSODesc desc;
			desc.vs = vertexShaders[VSTYPE_CUBE];
			desc.rs = rasterizers[RSTYPE_OCCLUDEE];
			desc.bs = blendStates[BSTYPE_COLORWRITEDISABLE];
			desc.dss = depthStencils[DSSTYPE_DEPTHREAD];
			desc.pt = TRIANGLESTRIP;

			desc.DSFormat = DSFormat_small;

			RECREATE(PSO_occlusionquery);
			device->CreateGraphicsPSO(&desc, PSO_occlusionquery);
		}
		for (int shaderType = 0; shaderType < SHADERTYPE_COUNT; ++shaderType)
		{
			const bool impostorRequest =
				shaderType != SHADERTYPE_VOXELIZE &&
				shaderType != SHADERTYPE_SHADOW &&
				shaderType != SHADERTYPE_SHADOWCUBE &&
				shaderType != SHADERTYPE_ENVMAPCAPTURE;
			if (!impostorRequest)
			{
				continue;
			}

			GraphicsPSODesc desc;
			desc.rs = rasterizers[RSTYPE_FRONT];
			desc.bs = blendStates[BSTYPE_OPAQUE];
			desc.dss = depthStencils[shaderType == SHADERTYPE_TILEDFORWARD ? DSSTYPE_DEPTHREADEQUAL : DSSTYPE_DEFAULT];

			VLTYPES realVL = (shaderType == SHADERTYPE_DEPTHONLY || shaderType == SHADERTYPE_TEXTURE) ? VLTYPE_OBJECT_POS_TEX : VLTYPE_OBJECT_ALL;
			VSTYPES realVS = realVL == VLTYPE_OBJECT_POS_TEX ? VSTYPE_OBJECT_SIMPLE : VSTYPE_OBJECT_COMMON;
			PSTYPES realPS;
			switch (shaderType)
			{
			case SHADERTYPE_DEFERRED:
				realPS = PSTYPE_OBJECT_DEFERRED_NORMALMAP;
				desc.numRTs = 4;
				desc.RTFormats[0] = RTFormat_gbuffer_0;
				desc.RTFormats[1] = RTFormat_gbuffer_1;
				desc.RTFormats[2] = RTFormat_gbuffer_2;
				desc.RTFormats[3] = RTFormat_gbuffer_3;
				break;
			case SHADERTYPE_FORWARD:
				realPS = PSTYPE_OBJECT_FORWARD_NORMALMAP;
				desc.numRTs = 2;
				desc.RTFormats[0] = RTFormat_hdr;
				desc.RTFormats[1] = RTFormat_gbuffer_1;
				break;
			case SHADERTYPE_TILEDFORWARD:
				realPS = PSTYPE_OBJECT_TILEDFORWARD_NORMALMAP;
				desc.numRTs = 2;
				desc.RTFormats[0] = RTFormat_hdr;
				desc.RTFormats[1] = RTFormat_gbuffer_1;
				break;
			case SHADERTYPE_DEPTHONLY:
				realPS = PSTYPE_OBJECT_ALPHATESTONLY;
				break;
			default:
				realPS = PSTYPE_OBJECT_TEXTUREONLY;
				desc.numRTs = 1;
				desc.RTFormats[0] = RTFormat_hdr;
				break;
			}
			desc.DSFormat = DSFormat_full;

			desc.vs = vertexShaders[realVS];
			desc.il = vertexLayouts[realVL];
			desc.ps = pixelShaders[realPS];

			RECREATE(PSO_impostor[shaderType]);
			device->CreateGraphicsPSO(&desc, PSO_impostor[shaderType]);
		}
		{
			GraphicsPSODesc desc;
			desc.vs = vertexShaders[VSTYPE_OBJECT_COMMON];
			desc.ps = pixelShaders[PSTYPE_CAPTUREIMPOSTOR];
			desc.rs = rasterizers[RSTYPE_DOUBLESIDED];
			desc.bs = blendStates[BSTYPE_OPAQUE];
			desc.dss = depthStencils[DSSTYPE_DEFAULT];
			desc.il = vertexLayouts[VLTYPE_OBJECT_ALL];

			desc.numRTs = 3;
			desc.RTFormats[0] = RTFormat_impostor_albedo;
			desc.RTFormats[1] = RTFormat_impostor_normal;
			desc.RTFormats[2] = RTFormat_impostor_surface;
			desc.DSFormat = DSFormat_full;

			RECREATE(PSO_captureimpostor);
			device->CreateGraphicsPSO(&desc, PSO_captureimpostor);
		}
	}));

	thread_pool.push_back(thread([&] {
		for (int type = 0; type < LightComponent::LIGHTTYPE_COUNT; ++type)
		{
			GraphicsPSODesc desc;

			// deferred lights:

			desc.pt = TRIANGLELIST;
			desc.rs = rasterizers[RSTYPE_BACK];
			desc.bs = blendStates[BSTYPE_DEFERREDLIGHT];

			switch (type)
			{
			case LightComponent::DIRECTIONAL:
				desc.vs = vertexShaders[VSTYPE_DIRLIGHT];
				desc.ps = pixelShaders[PSTYPE_DIRLIGHT];
				desc.dss = depthStencils[DSSTYPE_DIRLIGHT];
				break;
			case LightComponent::POINT:
				desc.vs = vertexShaders[VSTYPE_POINTLIGHT];
				desc.ps = pixelShaders[PSTYPE_POINTLIGHT];
				desc.dss = depthStencils[DSSTYPE_LIGHT];
				break;
			case LightComponent::SPOT:
				desc.vs = vertexShaders[VSTYPE_SPOTLIGHT];
				desc.ps = pixelShaders[PSTYPE_SPOTLIGHT];
				desc.dss = depthStencils[DSSTYPE_LIGHT];
				break;
			case LightComponent::SPHERE:
				desc.vs = vertexShaders[VSTYPE_DIRLIGHT];
				desc.ps = pixelShaders[PSTYPE_SPHERELIGHT];
				desc.dss = depthStencils[DSSTYPE_DIRLIGHT];
				break;
			case LightComponent::DISC:
				desc.vs = vertexShaders[VSTYPE_DIRLIGHT];
				desc.ps = pixelShaders[PSTYPE_DISCLIGHT];
				desc.dss = depthStencils[DSSTYPE_DIRLIGHT];
				break;
			case LightComponent::RECTANGLE:
				desc.vs = vertexShaders[VSTYPE_DIRLIGHT];
				desc.ps = pixelShaders[PSTYPE_RECTANGLELIGHT];
				desc.dss = depthStencils[DSSTYPE_DIRLIGHT];
				break;
			case LightComponent::TUBE:
				desc.vs = vertexShaders[VSTYPE_DIRLIGHT];
				desc.ps = pixelShaders[PSTYPE_TUBELIGHT];
				desc.dss = depthStencils[DSSTYPE_DIRLIGHT];
				break;
			}

			desc.numRTs = 2;
			desc.RTFormats[0] = RTFormat_deferred_lightbuffer;
			desc.RTFormats[1] = RTFormat_deferred_lightbuffer;
			desc.DSFormat = DSFormat_full;

			RECREATE(PSO_deferredlight[type]);
			device->CreateGraphicsPSO(&desc, PSO_deferredlight[type]);



			// light visualizers:
			if (type != LightComponent::DIRECTIONAL)
			{

				desc.dss = depthStencils[DSSTYPE_DEPTHREAD];
				desc.ps = pixelShaders[PSTYPE_LIGHTVISUALIZER];

				switch (type)
				{
				case LightComponent::POINT:
					desc.bs = blendStates[BSTYPE_ADDITIVE];
					desc.vs = vertexShaders[VSTYPE_LIGHTVISUALIZER_POINTLIGHT];
					desc.rs = rasterizers[RSTYPE_FRONT];
					break;
				case LightComponent::SPOT:
					desc.bs = blendStates[BSTYPE_ADDITIVE];
					desc.vs = vertexShaders[VSTYPE_LIGHTVISUALIZER_SPOTLIGHT];
					desc.rs = rasterizers[RSTYPE_DOUBLESIDED];
					break;
				case LightComponent::SPHERE:
					desc.bs = blendStates[BSTYPE_OPAQUE];
					desc.vs = vertexShaders[VSTYPE_LIGHTVISUALIZER_SPHERELIGHT];
					desc.rs = rasterizers[RSTYPE_FRONT];
					break;
				case LightComponent::DISC:
					desc.bs = blendStates[BSTYPE_OPAQUE];
					desc.vs = vertexShaders[VSTYPE_LIGHTVISUALIZER_DISCLIGHT];
					desc.rs = rasterizers[RSTYPE_FRONT];
					break;
				case LightComponent::RECTANGLE:
					desc.bs = blendStates[BSTYPE_OPAQUE];
					desc.vs = vertexShaders[VSTYPE_LIGHTVISUALIZER_RECTANGLELIGHT];
					desc.rs = rasterizers[RSTYPE_BACK];
					break;
				case LightComponent::TUBE:
					desc.bs = blendStates[BSTYPE_OPAQUE];
					desc.vs = vertexShaders[VSTYPE_LIGHTVISUALIZER_TUBELIGHT];
					desc.rs = rasterizers[RSTYPE_FRONT];
					break;
				}

				desc.numRTs = 1;
				desc.RTFormats[0] = RTFormat_hdr;
				desc.DSFormat = DSFormat_full;

				RECREATE(PSO_lightvisualizer[type]);
				device->CreateGraphicsPSO(&desc, PSO_lightvisualizer[type]);
			}


			// volumetric lights:
			if (type <= LightComponent::SPOT)
			{
				desc.dss = depthStencils[DSSTYPE_XRAY];
				desc.bs = blendStates[BSTYPE_ADDITIVE];
				desc.rs = rasterizers[RSTYPE_BACK];

				switch (type)
				{
				case LightComponent::DIRECTIONAL:
					desc.vs = vertexShaders[VSTYPE_DIRLIGHT];
					desc.ps = pixelShaders[PSTYPE_VOLUMETRICLIGHT_DIRECTIONAL];
					break;
				case LightComponent::POINT:
					desc.vs = vertexShaders[VSTYPE_POINTLIGHT];
					desc.ps = pixelShaders[PSTYPE_VOLUMETRICLIGHT_POINT];
					break;
				case LightComponent::SPOT:
					desc.vs = vertexShaders[VSTYPE_SPOTLIGHT];
					desc.ps = pixelShaders[PSTYPE_VOLUMETRICLIGHT_SPOT];
					break;
				}

				desc.numRTs = 1;
				desc.RTFormats[0] = RTFormat_hdr;
				desc.DSFormat = FORMAT_UNKNOWN;

				RECREATE(PSO_volumetriclight[type]);
				device->CreateGraphicsPSO(&desc, PSO_volumetriclight[type]);
			}


		}
		{
			GraphicsPSODesc desc;
			desc.vs = vertexShaders[VSTYPE_DIRLIGHT];
			desc.ps = pixelShaders[PSTYPE_ENVIRONMENTALLIGHT];
			desc.rs = rasterizers[RSTYPE_BACK];
			desc.bs = blendStates[BSTYPE_ENVIRONMENTALLIGHT];
			desc.dss = depthStencils[DSSTYPE_DIRLIGHT];

			desc.numRTs = 2;
			desc.RTFormats[0] = RTFormat_deferred_lightbuffer;
			desc.RTFormats[1] = RTFormat_deferred_lightbuffer;
			desc.DSFormat = DSFormat_full;

			RECREATE(PSO_enviromentallight);
			device->CreateGraphicsPSO(&desc, PSO_enviromentallight);
		}
		for (int type = 0; type < SKYRENDERING_COUNT; ++type)
		{
			GraphicsPSODesc desc;
			desc.rs = rasterizers[RSTYPE_SKY];
			desc.dss = depthStencils[DSSTYPE_DEPTHREAD];

			switch (type)
			{
			case SKYRENDERING_STATIC:
				desc.bs = blendStates[BSTYPE_OPAQUE];
				desc.vs = vertexShaders[VSTYPE_SKY];
				desc.ps = pixelShaders[PSTYPE_SKY_STATIC];
				desc.numRTs = 2;
				desc.RTFormats[0] = RTFormat_hdr;
				desc.RTFormats[1] = RTFormat_gbuffer_1;
				desc.DSFormat = DSFormat_full;
				break;
			case SKYRENDERING_DYNAMIC:
				desc.bs = blendStates[BSTYPE_OPAQUE];
				desc.vs = vertexShaders[VSTYPE_SKY];
				desc.ps = pixelShaders[PSTYPE_SKY_DYNAMIC];
				desc.numRTs = 2;
				desc.RTFormats[0] = RTFormat_hdr;
				desc.RTFormats[1] = RTFormat_gbuffer_1;
				desc.DSFormat = DSFormat_full;
				break;
			case SKYRENDERING_SUN:
				desc.bs = blendStates[BSTYPE_ADDITIVE];
				desc.vs = vertexShaders[VSTYPE_SKY];
				desc.ps = pixelShaders[PSTYPE_SUN];
				desc.numRTs = 1;
				desc.RTFormats[0] = RTFormat_hdr;
				desc.DSFormat = DSFormat_full;
				break;
			case SKYRENDERING_ENVMAPCAPTURE_STATIC:
				desc.bs = blendStates[BSTYPE_OPAQUE];
				desc.vs = vertexShaders[VSTYPE_ENVMAP_SKY];
				desc.ps = pixelShaders[PSTYPE_ENVMAP_SKY_STATIC];
				desc.gs = geometryShaders[GSTYPE_ENVMAP_SKY];
				desc.numRTs = 1;
				desc.RTFormats[0] = RTFormat_hdr;
				desc.DSFormat = DSFormat_small;
				break;
			case SKYRENDERING_ENVMAPCAPTURE_DYNAMIC:
				desc.bs = blendStates[BSTYPE_OPAQUE];
				desc.vs = vertexShaders[VSTYPE_ENVMAP_SKY];
				desc.ps = pixelShaders[PSTYPE_ENVMAP_SKY_DYNAMIC];
				desc.gs = geometryShaders[GSTYPE_ENVMAP_SKY];
				desc.numRTs = 1;
				desc.RTFormats[0] = RTFormat_hdr;
				desc.DSFormat = DSFormat_small;
				break;
			}

			RECREATE(PSO_sky[type]);
			device->CreateGraphicsPSO(&desc, PSO_sky[type]);
		}
		for (int debug = 0; debug < DEBUGRENDERING_COUNT; ++debug)
		{
			GraphicsPSODesc desc;

			switch (debug)
			{
			case DEBUGRENDERING_ENVPROBE:
				desc.vs = vertexShaders[VSTYPE_SPHERE];
				desc.ps = pixelShaders[PSTYPE_CUBEMAP];
				desc.dss = depthStencils[DSSTYPE_DEFAULT];
				desc.rs = rasterizers[RSTYPE_FRONT];
				desc.bs = blendStates[BSTYPE_OPAQUE];
				desc.pt = TRIANGLELIST;
				break;
			case DEBUGRENDERING_GRID:
				desc.vs = vertexShaders[VSTYPE_LINE];
				desc.ps = pixelShaders[PSTYPE_LINE];
				desc.il = vertexLayouts[VLTYPE_LINE];
				desc.dss = depthStencils[DSSTYPE_DEPTHREAD];
				desc.rs = rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
				desc.bs = blendStates[BSTYPE_TRANSPARENT];
				desc.pt = LINELIST;
				break;
			case DEBUGRENDERING_CUBE:
				desc.vs = vertexShaders[VSTYPE_LINE];
				desc.ps = pixelShaders[PSTYPE_LINE];
				desc.il = vertexLayouts[VLTYPE_LINE];
				desc.dss = depthStencils[DSSTYPE_DEPTHREAD];
				desc.rs = rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
				desc.bs = blendStates[BSTYPE_TRANSPARENT];
				desc.pt = LINELIST;
				break;
			case DEBUGRENDERING_LINES:
				desc.vs = vertexShaders[VSTYPE_LINE];
				desc.ps = pixelShaders[PSTYPE_LINE];
				desc.il = vertexLayouts[VLTYPE_LINE];
				desc.dss = depthStencils[DSSTYPE_XRAY];
				desc.rs = rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
				desc.bs = blendStates[BSTYPE_TRANSPARENT];
				desc.pt = LINELIST;
				break;
			case DEBUGRENDERING_EMITTER:
				desc.vs = vertexShaders[VSTYPE_OBJECT_DEBUG];
				desc.ps = pixelShaders[PSTYPE_OBJECT_DEBUG];
				desc.il = vertexLayouts[VLTYPE_OBJECT_DEBUG];
				desc.dss = depthStencils[DSSTYPE_DEPTHREAD];
				desc.rs = rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
				desc.bs = blendStates[BSTYPE_OPAQUE];
				desc.pt = TRIANGLELIST;
				break;
			case DEBUGRENDERING_VOXEL:
				desc.vs = vertexShaders[VSTYPE_VOXEL];
				desc.ps = pixelShaders[PSTYPE_VOXEL];
				desc.gs = geometryShaders[GSTYPE_VOXEL];
				desc.dss = depthStencils[DSSTYPE_DEFAULT];
				desc.rs = rasterizers[RSTYPE_BACK];
				desc.bs = blendStates[BSTYPE_OPAQUE];
				desc.pt = POINTLIST;
				break;
			case DEBUGRENDERING_FORCEFIELD_POINT:
				desc.vs = vertexShaders[VSTYPE_FORCEFIELDVISUALIZER_POINT];
				desc.ps = pixelShaders[PSTYPE_FORCEFIELDVISUALIZER];
				desc.dss = depthStencils[DSSTYPE_XRAY];
				desc.rs = rasterizers[RSTYPE_BACK];
				desc.bs = blendStates[BSTYPE_TRANSPARENT];
				desc.pt = TRIANGLELIST;
				break;
			case DEBUGRENDERING_FORCEFIELD_PLANE:
				desc.vs = vertexShaders[VSTYPE_FORCEFIELDVISUALIZER_PLANE];
				desc.ps = pixelShaders[PSTYPE_FORCEFIELDVISUALIZER];
				desc.dss = depthStencils[DSSTYPE_XRAY];
				desc.rs = rasterizers[RSTYPE_FRONT];
				desc.bs = blendStates[BSTYPE_TRANSPARENT];
				desc.pt = TRIANGLESTRIP;
				break;
			}

			desc.numRTs = 1;
			desc.RTFormats[0] = RTFormat_hdr;
			desc.DSFormat = DSFormat_full;

			RECREATE(PSO_debug[debug]);
			device->CreateGraphicsPSO(&desc, PSO_debug[debug]);
		}
	}));


	thread_pool.push_back(thread([&] {
		for (int i = 0; i < TILEDLIGHTING_TYPE_COUNT; ++i)
		{
			for (int j = 0; j < TILEDLIGHTING_CULLING_COUNT; ++j)
			{
				for (int k = 0; k < TILEDLIGHTING_DEBUG_COUNT; ++k)
				{
					string name = "lightCullingCS";
					if (i == TILEDLIGHTING_TYPE_DEFERRED)
					{
						name += "_DEFERRED";
					}
					if (j == TILEDLIGHTING_CULLING_ADVANCED)
					{
						name += "_ADVANCED";
					}
					if (k == TILEDLIGHTING_DEBUG_ENABLED)
					{
						name += "_DEBUG";
					}
					name += ".cso";

					ComputePSODesc desc;
					desc.cs = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(SHADERPATH + name, wiResourceManager::COMPUTESHADER));
					RECREATE(CPSO_tiledlighting[i][j][k]);
					device->CreateComputePSO(&desc, CPSO_tiledlighting[i][j][k]);
				}
			}
		}

		for (int i = 0; i < CSTYPE_LAST; ++i)
		{
			ComputePSODesc desc;
			desc.cs = computeShaders[i];
			RECREATE(CPSO[i]);
			device->CreateComputePSO(&desc, CPSO[i]);
		}
	}));


	for (auto& x : thread_pool)
	{
		x.join();
	}
	thread_pool.clear();
}

void wiRenderer::ReloadShaders(const std::string& path)
{

	if (path.length() > 0)
	{
		SHADERPATH = path;
	}

	GetDevice()->WaitForGPU();

	wiResourceManager::GetShaderManager()->CleanUp();
	LoadShaders();
	wiHairParticle::LoadShaders();
	wiEmittedParticle::LoadShaders();
	wiFont::LoadShaders();
	wiImage::LoadShaders();
	wiLensFlare::LoadShaders();
	wiOcean::LoadShaders();
	CSFFT_512x512_Data_t::LoadShaders();
	wiWidget::LoadShaders();
	wiGPUSortLib::LoadShaders();
}


void wiRenderer::SetUpStates()
{
	for (int i = 0; i < SSLOT_COUNT; ++i)
	{
		samplers[i] = new Sampler;
	}

	SamplerDesc samplerDesc;
	samplerDesc.Filter = FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = TEXTURE_ADDRESS_MIRROR;
	samplerDesc.AddressV = TEXTURE_ADDRESS_MIRROR;
	samplerDesc.AddressW = TEXTURE_ADDRESS_MIRROR;
	samplerDesc.MipLODBias = 0.0f;
	samplerDesc.MaxAnisotropy = 0;
	samplerDesc.ComparisonFunc = COMPARISON_NEVER;
	samplerDesc.BorderColor[0] = 0;
	samplerDesc.BorderColor[1] = 0;
	samplerDesc.BorderColor[2] = 0;
	samplerDesc.BorderColor[3] = 0;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = FLOAT32_MAX;
	GetDevice()->CreateSamplerState(&samplerDesc, samplers[SSLOT_LINEAR_MIRROR]);

	samplerDesc.Filter = FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = TEXTURE_ADDRESS_CLAMP;
	GetDevice()->CreateSamplerState(&samplerDesc, samplers[SSLOT_LINEAR_CLAMP]);

	samplerDesc.Filter = FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressV = TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressW = TEXTURE_ADDRESS_WRAP;
	GetDevice()->CreateSamplerState(&samplerDesc, samplers[SSLOT_LINEAR_WRAP]);
	
	samplerDesc.Filter = FILTER_MIN_MAG_MIP_POINT;
	samplerDesc.AddressU = TEXTURE_ADDRESS_MIRROR;
	samplerDesc.AddressV = TEXTURE_ADDRESS_MIRROR;
	samplerDesc.AddressW = TEXTURE_ADDRESS_MIRROR;
	GetDevice()->CreateSamplerState(&samplerDesc, samplers[SSLOT_POINT_MIRROR]);
	
	samplerDesc.Filter = FILTER_MIN_MAG_MIP_POINT;
	samplerDesc.AddressU = TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressV = TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressW = TEXTURE_ADDRESS_WRAP;
	GetDevice()->CreateSamplerState(&samplerDesc, samplers[SSLOT_POINT_WRAP]);
	
	
	samplerDesc.Filter = FILTER_MIN_MAG_MIP_POINT;
	samplerDesc.AddressU = TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = TEXTURE_ADDRESS_CLAMP;
	GetDevice()->CreateSamplerState(&samplerDesc, samplers[SSLOT_POINT_CLAMP]);
	
	samplerDesc.Filter = FILTER_ANISOTROPIC;
	samplerDesc.AddressU = TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxAnisotropy = 16;
	GetDevice()->CreateSamplerState(&samplerDesc, samplers[SSLOT_ANISO_CLAMP]);
	
	samplerDesc.Filter = FILTER_ANISOTROPIC;
	samplerDesc.AddressU = TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressV = TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressW = TEXTURE_ADDRESS_WRAP;
	samplerDesc.MaxAnisotropy = 16;
	GetDevice()->CreateSamplerState(&samplerDesc, samplers[SSLOT_ANISO_WRAP]);
	
	samplerDesc.Filter = FILTER_ANISOTROPIC;
	samplerDesc.AddressU = TEXTURE_ADDRESS_MIRROR;
	samplerDesc.AddressV = TEXTURE_ADDRESS_MIRROR;
	samplerDesc.AddressW = TEXTURE_ADDRESS_MIRROR;
	samplerDesc.MaxAnisotropy = 16;
	GetDevice()->CreateSamplerState(&samplerDesc, samplers[SSLOT_ANISO_MIRROR]);

	samplerDesc.Filter = FILTER_ANISOTROPIC;
	samplerDesc.AddressU = TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressV = TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressW = TEXTURE_ADDRESS_WRAP;
	samplerDesc.MaxAnisotropy = 16;
	GetDevice()->CreateSamplerState(&samplerDesc, samplers[SSLOT_OBJECTSHADER]);

	ZeroMemory( &samplerDesc, sizeof(SamplerDesc) );
	samplerDesc.Filter = FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
	samplerDesc.AddressU = TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MipLODBias = 0.0f;
	samplerDesc.MaxAnisotropy = 0;
	samplerDesc.ComparisonFunc = COMPARISON_GREATER_EQUAL;
	GetDevice()->CreateSamplerState(&samplerDesc, samplers[SSLOT_CMP_DEPTH]);

	for (int i = 0; i < SSTYPE_LAST; ++i)
	{
		customsamplers[i] = new Sampler;
	}

	samplerDesc.Filter = FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MipLODBias = 0.0f;
	samplerDesc.MaxAnisotropy = 0;
	samplerDesc.ComparisonFunc = COMPARISON_NEVER;
	samplerDesc.BorderColor[0] = 0;
	samplerDesc.BorderColor[1] = 0;
	samplerDesc.BorderColor[2] = 0;
	samplerDesc.BorderColor[3] = 0;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = FLOAT32_MAX;
	GetDevice()->CreateSamplerState(&samplerDesc, customsamplers[SSTYPE_MAXIMUM_CLAMP]);


	for (int i = 0; i < RSTYPE_LAST; ++i)
	{
		rasterizers[i] = new RasterizerState;
	}
	
	RasterizerStateDesc rs;
	rs.FillMode=FILL_SOLID;
	rs.CullMode=CULL_BACK;
	rs.FrontCounterClockwise=true;
	rs.DepthBias=0;
	rs.DepthBiasClamp=0;
	rs.SlopeScaledDepthBias=0;
	rs.DepthClipEnable=true;
	rs.MultisampleEnable=false;
	rs.AntialiasedLineEnable=false;
	rs.ConservativeRasterizationEnable = false;
	GetDevice()->CreateRasterizerState(&rs,rasterizers[RSTYPE_FRONT]);

	
	rs.FillMode=FILL_SOLID;
	rs.CullMode=CULL_BACK;
	rs.FrontCounterClockwise=true;
	rs.DepthBias=0;
	rs.DepthBiasClamp=0;
	rs.SlopeScaledDepthBias = -2.0f;
	rs.DepthClipEnable=true;
	rs.MultisampleEnable=false;
	rs.AntialiasedLineEnable=false;
	rs.ConservativeRasterizationEnable = false;
	GetDevice()->CreateRasterizerState(&rs,rasterizers[RSTYPE_SHADOW]);

	rs.FillMode=FILL_SOLID;
	rs.CullMode=CULL_NONE;
	rs.FrontCounterClockwise=true;
	rs.DepthBias=0;
	rs.DepthBiasClamp=0;
	rs.SlopeScaledDepthBias = -2.0f;
	rs.DepthClipEnable=true;
	rs.MultisampleEnable=false;
	rs.AntialiasedLineEnable=false;
	rs.ConservativeRasterizationEnable = false;
	GetDevice()->CreateRasterizerState(&rs,rasterizers[RSTYPE_SHADOW_DOUBLESIDED]);

	rs.FillMode=FILL_WIREFRAME;
	rs.CullMode=CULL_BACK;
	rs.FrontCounterClockwise=true;
	rs.DepthBias=0;
	rs.DepthBiasClamp=0;
	rs.SlopeScaledDepthBias=0;
	rs.DepthClipEnable=true;
	rs.MultisampleEnable=false;
	rs.AntialiasedLineEnable = false;
	rs.ConservativeRasterizationEnable = false;
	GetDevice()->CreateRasterizerState(&rs, rasterizers[RSTYPE_WIRE]);
	rs.AntialiasedLineEnable = true;
	GetDevice()->CreateRasterizerState(&rs, rasterizers[RSTYPE_WIRE_SMOOTH]);
	
	rs.FillMode=FILL_SOLID;
	rs.CullMode=CULL_NONE;
	rs.FrontCounterClockwise=true;
	rs.DepthBias=0;
	rs.DepthBiasClamp=0;
	rs.SlopeScaledDepthBias=0;
	rs.DepthClipEnable=true;
	rs.MultisampleEnable=false;
	rs.AntialiasedLineEnable=false;
	rs.ConservativeRasterizationEnable = false;
	GetDevice()->CreateRasterizerState(&rs,rasterizers[RSTYPE_DOUBLESIDED]);
	
	rs.FillMode=FILL_WIREFRAME;
	rs.CullMode=CULL_NONE;
	rs.FrontCounterClockwise=true;
	rs.DepthBias = 0;
	rs.DepthBiasClamp = 0;
	rs.SlopeScaledDepthBias=0;
	rs.DepthClipEnable=true;
	rs.MultisampleEnable=false;
	rs.AntialiasedLineEnable = false;
	rs.ConservativeRasterizationEnable = false;
	GetDevice()->CreateRasterizerState(&rs, rasterizers[RSTYPE_WIRE_DOUBLESIDED]);
	rs.AntialiasedLineEnable = true;
	GetDevice()->CreateRasterizerState(&rs, rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH]);
	
	rs.FillMode=FILL_SOLID;
	rs.CullMode=CULL_FRONT;
	rs.FrontCounterClockwise=true;
	rs.DepthBias=0;
	rs.DepthBiasClamp=0;
	rs.SlopeScaledDepthBias=0;
	rs.DepthClipEnable=true;
	rs.MultisampleEnable=false;
	rs.AntialiasedLineEnable=false;
	rs.ConservativeRasterizationEnable = false;
	GetDevice()->CreateRasterizerState(&rs,rasterizers[RSTYPE_BACK]);

	rs.FillMode = FILL_SOLID;
	rs.CullMode = CULL_FRONT;
	rs.FrontCounterClockwise = true;
	rs.DepthBias = 0;
	rs.DepthBiasClamp = 0;
	rs.SlopeScaledDepthBias = 0;
	rs.DepthClipEnable = true;
	rs.MultisampleEnable = false;
	rs.AntialiasedLineEnable = false;
	rs.ConservativeRasterizationEnable = false;
	GetDevice()->CreateRasterizerState(&rs, rasterizers[RSTYPE_OCCLUDEE]);

	rs.FillMode = FILL_SOLID;
	rs.CullMode = CULL_FRONT;
	rs.FrontCounterClockwise = true;
	rs.DepthBias = 0;
	rs.DepthBiasClamp = 0;
	rs.SlopeScaledDepthBias = 0;
	rs.DepthClipEnable = false;
	rs.MultisampleEnable = false;
	rs.AntialiasedLineEnable = false;
	rs.ConservativeRasterizationEnable = false;
	GetDevice()->CreateRasterizerState(&rs, rasterizers[RSTYPE_SKY]);

	rs.FillMode = FILL_SOLID;
	rs.CullMode = CULL_NONE;
	rs.FrontCounterClockwise = true;
	rs.DepthBias = 0;
	rs.DepthBiasClamp = 0;
	rs.SlopeScaledDepthBias = 0;
	rs.DepthClipEnable = true;
	rs.MultisampleEnable = false;
	rs.AntialiasedLineEnable = false;
	rs.ConservativeRasterizationEnable = false; // do it in the shader for now...
	GetDevice()->CreateRasterizerState(&rs, rasterizers[RSTYPE_VOXELIZE]);

	for (int i = 0; i < DSSTYPE_LAST; ++i)
	{
		depthStencils[i] = new DepthStencilState;
	}

	DepthStencilStateDesc dsd;
	dsd.DepthEnable = true;
	dsd.DepthWriteMask = DEPTH_WRITE_MASK_ALL;
	dsd.DepthFunc = COMPARISON_GREATER;

	dsd.StencilEnable = true;
	dsd.StencilReadMask = 0xFF;
	dsd.StencilWriteMask = 0xFF;
	dsd.FrontFace.StencilFunc = COMPARISON_ALWAYS;
	dsd.FrontFace.StencilPassOp = STENCIL_OP_REPLACE;
	dsd.FrontFace.StencilFailOp = STENCIL_OP_KEEP;
	dsd.FrontFace.StencilDepthFailOp = STENCIL_OP_KEEP;
	dsd.BackFace.StencilFunc = COMPARISON_ALWAYS;
	dsd.BackFace.StencilPassOp = STENCIL_OP_REPLACE;
	dsd.BackFace.StencilFailOp = STENCIL_OP_KEEP;
	dsd.BackFace.StencilDepthFailOp = STENCIL_OP_KEEP;
	GetDevice()->CreateDepthStencilState(&dsd, depthStencils[DSSTYPE_DEFAULT]);

	dsd.DepthEnable = true;
	dsd.DepthWriteMask = DEPTH_WRITE_MASK_ALL;
	dsd.DepthFunc = COMPARISON_GREATER;
	dsd.StencilEnable = false;
	GetDevice()->CreateDepthStencilState(&dsd, depthStencils[DSSTYPE_SHADOW]);


	dsd.DepthWriteMask = DEPTH_WRITE_MASK_ZERO;
	dsd.DepthEnable = false;
	dsd.DepthFunc = COMPARISON_LESS;
	dsd.StencilEnable = false;
	dsd.StencilReadMask = 0xFF;
	dsd.StencilWriteMask = 0xFF;
	dsd.FrontFace.StencilFunc = COMPARISON_LESS;
	dsd.FrontFace.StencilPassOp = STENCIL_OP_KEEP;
	dsd.FrontFace.StencilFailOp = STENCIL_OP_KEEP;
	dsd.FrontFace.StencilDepthFailOp = STENCIL_OP_KEEP;
	dsd.BackFace.StencilFunc = COMPARISON_LESS_EQUAL;
	dsd.BackFace.StencilPassOp = STENCIL_OP_KEEP;
	dsd.BackFace.StencilFailOp = STENCIL_OP_KEEP;
	dsd.BackFace.StencilDepthFailOp = STENCIL_OP_KEEP;
	GetDevice()->CreateDepthStencilState(&dsd, depthStencils[DSSTYPE_DIRLIGHT]);


	dsd.DepthWriteMask = DEPTH_WRITE_MASK_ZERO;
	dsd.DepthEnable = true;
	dsd.DepthFunc = COMPARISON_LESS;
	dsd.StencilEnable = false;
	dsd.StencilReadMask = 0xFF;
	dsd.StencilWriteMask = 0xFF;
	dsd.FrontFace.StencilFunc = COMPARISON_LESS_EQUAL;
	dsd.FrontFace.StencilPassOp = STENCIL_OP_KEEP;
	dsd.FrontFace.StencilFailOp = STENCIL_OP_KEEP;
	dsd.FrontFace.StencilDepthFailOp = STENCIL_OP_KEEP;
	dsd.BackFace.StencilFunc = COMPARISON_LESS_EQUAL;
	dsd.BackFace.StencilPassOp = STENCIL_OP_KEEP;
	dsd.BackFace.StencilFailOp = STENCIL_OP_KEEP;
	dsd.BackFace.StencilDepthFailOp = STENCIL_OP_KEEP;
	GetDevice()->CreateDepthStencilState(&dsd, depthStencils[DSSTYPE_LIGHT]);


	dsd.DepthWriteMask = DEPTH_WRITE_MASK_ZERO;
	dsd.DepthEnable = true;
	dsd.StencilEnable = true;
	dsd.DepthFunc = COMPARISON_LESS;
	dsd.StencilReadMask = 0xFF;
	dsd.StencilWriteMask = 0x00;
	dsd.FrontFace.StencilFunc = COMPARISON_EQUAL;
	dsd.FrontFace.StencilPassOp = STENCIL_OP_KEEP;
	dsd.FrontFace.StencilFailOp = STENCIL_OP_KEEP;
	dsd.FrontFace.StencilDepthFailOp = STENCIL_OP_KEEP;
	dsd.BackFace.StencilFunc = COMPARISON_EQUAL;
	dsd.BackFace.StencilPassOp = STENCIL_OP_KEEP;
	dsd.BackFace.StencilFailOp = STENCIL_OP_KEEP;
	dsd.BackFace.StencilDepthFailOp = STENCIL_OP_KEEP;
	GetDevice()->CreateDepthStencilState(&dsd, depthStencils[DSSTYPE_DECAL]);


	dsd.DepthWriteMask = DEPTH_WRITE_MASK_ALL;
	dsd.DepthEnable = false;
	dsd.StencilEnable = true;
	dsd.DepthFunc = COMPARISON_GREATER;
	dsd.StencilReadMask = 0xFF;
	dsd.StencilWriteMask = 0x00;
	dsd.FrontFace.StencilFunc = COMPARISON_EQUAL;
	dsd.FrontFace.StencilPassOp = STENCIL_OP_KEEP;
	dsd.FrontFace.StencilFailOp = STENCIL_OP_KEEP;
	dsd.FrontFace.StencilDepthFailOp = STENCIL_OP_KEEP;
	dsd.BackFace.StencilFunc = COMPARISON_EQUAL;
	dsd.BackFace.StencilPassOp = STENCIL_OP_KEEP;
	dsd.BackFace.StencilFailOp = STENCIL_OP_KEEP;
	dsd.BackFace.StencilDepthFailOp = STENCIL_OP_KEEP;
	GetDevice()->CreateDepthStencilState(&dsd, depthStencils[DSSTYPE_STENCILREAD_MATCH]);


	dsd.DepthEnable = true;
	dsd.StencilEnable = false;
	dsd.DepthWriteMask = DEPTH_WRITE_MASK_ZERO;
	dsd.DepthFunc = COMPARISON_GREATER_EQUAL;
	GetDevice()->CreateDepthStencilState(&dsd, depthStencils[DSSTYPE_DEPTHREAD]);

	dsd.DepthEnable = false;
	dsd.StencilEnable=false;
	GetDevice()->CreateDepthStencilState(&dsd, depthStencils[DSSTYPE_XRAY]);


	dsd.DepthEnable = true;
	dsd.DepthWriteMask = DEPTH_WRITE_MASK_ZERO;
	dsd.DepthFunc = COMPARISON_EQUAL;
	GetDevice()->CreateDepthStencilState(&dsd, depthStencils[DSSTYPE_DEPTHREADEQUAL]);


	dsd.DepthEnable = true;
	dsd.DepthWriteMask = DEPTH_WRITE_MASK_ALL;
	dsd.DepthFunc = COMPARISON_GREATER;
	GetDevice()->CreateDepthStencilState(&dsd, depthStencils[DSSTYPE_ENVMAP]);


	for (int i = 0; i < BSTYPE_LAST; ++i)
	{
		blendStates[i] = new BlendState;
	}
	
	BlendStateDesc bd;
	ZeroMemory(&bd, sizeof(bd));
	bd.RenderTarget[0].BlendEnable=false;
	bd.RenderTarget[0].SrcBlend = BLEND_SRC_ALPHA;
	bd.RenderTarget[0].DestBlend = BLEND_INV_SRC_ALPHA;
	bd.RenderTarget[0].BlendOp = BLEND_OP_MAX;
	bd.RenderTarget[0].SrcBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].DestBlendAlpha = BLEND_ZERO;
	bd.RenderTarget[0].BlendOpAlpha = BLEND_OP_ADD;
	bd.RenderTarget[0].RenderTargetWriteMask = COLOR_WRITE_ENABLE_ALL;
	bd.AlphaToCoverageEnable=false;
	bd.IndependentBlendEnable = false;
	GetDevice()->CreateBlendState(&bd,blendStates[BSTYPE_OPAQUE]);

	bd.RenderTarget[0].SrcBlend = BLEND_SRC_ALPHA;
	bd.RenderTarget[0].DestBlend = BLEND_INV_SRC_ALPHA;
	bd.RenderTarget[0].BlendOp = BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].DestBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].BlendOpAlpha = BLEND_OP_ADD;
	bd.RenderTarget[0].BlendEnable=true;
	bd.RenderTarget[0].RenderTargetWriteMask = COLOR_WRITE_ENABLE_ALL;
	bd.AlphaToCoverageEnable = false;
	bd.IndependentBlendEnable = false;
	GetDevice()->CreateBlendState(&bd,blendStates[BSTYPE_TRANSPARENT]);


	bd.RenderTarget[0].BlendEnable=true;
	bd.RenderTarget[0].SrcBlend = BLEND_SRC_ALPHA;
	bd.RenderTarget[0].DestBlend = BLEND_ONE;
	bd.RenderTarget[0].BlendOp = BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = BLEND_ZERO;
	bd.RenderTarget[0].DestBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].BlendOpAlpha = BLEND_OP_ADD;
	bd.RenderTarget[0].RenderTargetWriteMask = COLOR_WRITE_ENABLE_ALL;
	bd.IndependentBlendEnable=false,
	bd.AlphaToCoverageEnable=false;
	GetDevice()->CreateBlendState(&bd,blendStates[BSTYPE_ADDITIVE]);


	bd.RenderTarget[0].BlendEnable = false;
	bd.RenderTarget[0].RenderTargetWriteMask = COLOR_WRITE_DISABLE;
	bd.IndependentBlendEnable = false,
	bd.AlphaToCoverageEnable = false;
	GetDevice()->CreateBlendState(&bd, blendStates[BSTYPE_COLORWRITEDISABLE]);


	bd.RenderTarget[0].BlendEnable=true;
	bd.RenderTarget[0].SrcBlend = BLEND_ONE;
	bd.RenderTarget[0].DestBlend = BLEND_ONE;
	bd.RenderTarget[0].BlendOp = BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].DestBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].BlendOpAlpha = BLEND_OP_ADD;
	bd.RenderTarget[0].RenderTargetWriteMask = COLOR_WRITE_ENABLE_RED | COLOR_WRITE_ENABLE_GREEN | COLOR_WRITE_ENABLE_BLUE; // alpha is not written by deferred lights!
	bd.IndependentBlendEnable=false,
	bd.AlphaToCoverageEnable=false;
	GetDevice()->CreateBlendState(&bd,blendStates[BSTYPE_DEFERREDLIGHT]);

	bd.RenderTarget[0].BlendEnable = true;
	bd.RenderTarget[0].SrcBlend = BLEND_ONE;
	bd.RenderTarget[0].DestBlend = BLEND_ONE;
	bd.RenderTarget[0].BlendOp = BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].DestBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].BlendOpAlpha = BLEND_OP_ADD;
	bd.RenderTarget[0].RenderTargetWriteMask = COLOR_WRITE_ENABLE_ALL;
	bd.IndependentBlendEnable = false,
	bd.AlphaToCoverageEnable = false;
	GetDevice()->CreateBlendState(&bd, blendStates[BSTYPE_ENVIRONMENTALLIGHT]);

	bd.RenderTarget[0].SrcBlend = BLEND_INV_SRC_COLOR;
	bd.RenderTarget[0].DestBlend = BLEND_INV_DEST_COLOR;
	bd.RenderTarget[0].BlendOp = BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].DestBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].BlendOpAlpha = BLEND_OP_ADD;
	bd.RenderTarget[0].BlendEnable = true;
	bd.RenderTarget[0].RenderTargetWriteMask = COLOR_WRITE_ENABLE_ALL;
	bd.AlphaToCoverageEnable = false;
	bd.IndependentBlendEnable = false;
	GetDevice()->CreateBlendState(&bd, blendStates[BSTYPE_INVERSE]);


	bd.RenderTarget[0].SrcBlend = BLEND_SRC_ALPHA;
	bd.RenderTarget[0].DestBlend = BLEND_INV_SRC_ALPHA;
	bd.RenderTarget[0].BlendOp = BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].DestBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].BlendOpAlpha = BLEND_OP_ADD;
	bd.RenderTarget[0].BlendEnable = true;
	bd.RenderTarget[0].RenderTargetWriteMask = COLOR_WRITE_ENABLE_RED | COLOR_WRITE_ENABLE_GREEN | COLOR_WRITE_ENABLE_BLUE;
	bd.RenderTarget[1] = bd.RenderTarget[0];
	bd.RenderTarget[1].RenderTargetWriteMask = COLOR_WRITE_ENABLE_RED | COLOR_WRITE_ENABLE_GREEN;
	bd.AlphaToCoverageEnable = false;
	bd.IndependentBlendEnable = true;
	GetDevice()->CreateBlendState(&bd, blendStates[BSTYPE_DECAL]);


	bd.RenderTarget[0].SrcBlend = BLEND_DEST_COLOR;
	bd.RenderTarget[0].DestBlend = BLEND_ZERO;
	bd.RenderTarget[0].BlendOp = BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = BLEND_DEST_ALPHA;
	bd.RenderTarget[0].DestBlendAlpha = BLEND_ZERO;
	bd.RenderTarget[0].BlendOpAlpha = BLEND_OP_ADD;
	bd.RenderTarget[0].BlendEnable = true;
	bd.RenderTarget[0].RenderTargetWriteMask = COLOR_WRITE_ENABLE_ALL;
	bd.AlphaToCoverageEnable = false;
	bd.IndependentBlendEnable = false;
	GetDevice()->CreateBlendState(&bd, blendStates[BSTYPE_MULTIPLY]);


	bd.RenderTarget[0].SrcBlend = BLEND_DEST_COLOR;
	bd.RenderTarget[0].DestBlend = BLEND_ZERO;
	bd.RenderTarget[0].BlendOp = BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].DestBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].BlendOpAlpha = BLEND_OP_ADD;
	bd.RenderTarget[0].BlendEnable = true;
	bd.RenderTarget[0].RenderTargetWriteMask = COLOR_WRITE_ENABLE_ALL;
	bd.AlphaToCoverageEnable = false;
	bd.IndependentBlendEnable = false;
	GetDevice()->CreateBlendState(&bd, blendStates[BSTYPE_TRANSPARENTSHADOWMAP]);
}

void wiRenderer::BindPersistentState(GRAPHICSTHREAD threadID)
{
	GraphicsDevice* device = GetDevice();

	for (int i = 0; i < SSLOT_COUNT; ++i)
	{
		device->BindSampler(PS, samplers[i], i, threadID);
		device->BindSampler(VS, samplers[i], i, threadID);
		device->BindSampler(GS, samplers[i], i, threadID);
		device->BindSampler(DS, samplers[i], i, threadID);
		device->BindSampler(HS, samplers[i], i, threadID);
		device->BindSampler(CS, samplers[i], i, threadID);
	}


	device->BindConstantBuffer(PS, constantBuffers[CBTYPE_WORLD], CB_GETBINDSLOT(WorldCB), threadID);
	device->BindConstantBuffer(VS, constantBuffers[CBTYPE_WORLD], CB_GETBINDSLOT(WorldCB), threadID);
	device->BindConstantBuffer(GS, constantBuffers[CBTYPE_WORLD], CB_GETBINDSLOT(WorldCB), threadID);
	device->BindConstantBuffer(HS, constantBuffers[CBTYPE_WORLD], CB_GETBINDSLOT(WorldCB), threadID);
	device->BindConstantBuffer(DS, constantBuffers[CBTYPE_WORLD], CB_GETBINDSLOT(WorldCB), threadID);
	device->BindConstantBuffer(CS, constantBuffers[CBTYPE_WORLD], CB_GETBINDSLOT(WorldCB), threadID);

	device->BindConstantBuffer(PS, constantBuffers[CBTYPE_FRAME], CB_GETBINDSLOT(FrameCB), threadID);
	device->BindConstantBuffer(VS, constantBuffers[CBTYPE_FRAME], CB_GETBINDSLOT(FrameCB), threadID);
	device->BindConstantBuffer(GS, constantBuffers[CBTYPE_FRAME], CB_GETBINDSLOT(FrameCB), threadID);
	device->BindConstantBuffer(HS, constantBuffers[CBTYPE_FRAME], CB_GETBINDSLOT(FrameCB), threadID);
	device->BindConstantBuffer(DS, constantBuffers[CBTYPE_FRAME], CB_GETBINDSLOT(FrameCB), threadID);
	device->BindConstantBuffer(CS, constantBuffers[CBTYPE_FRAME], CB_GETBINDSLOT(FrameCB), threadID);

	device->BindConstantBuffer(PS, constantBuffers[CBTYPE_CAMERA], CB_GETBINDSLOT(CameraCB), threadID);
	device->BindConstantBuffer(VS, constantBuffers[CBTYPE_CAMERA], CB_GETBINDSLOT(CameraCB), threadID);
	device->BindConstantBuffer(GS, constantBuffers[CBTYPE_CAMERA], CB_GETBINDSLOT(CameraCB), threadID);
	device->BindConstantBuffer(HS, constantBuffers[CBTYPE_CAMERA], CB_GETBINDSLOT(CameraCB), threadID);
	device->BindConstantBuffer(DS, constantBuffers[CBTYPE_CAMERA], CB_GETBINDSLOT(CameraCB), threadID);
	device->BindConstantBuffer(CS, constantBuffers[CBTYPE_CAMERA], CB_GETBINDSLOT(CameraCB), threadID);

	device->BindConstantBuffer(VS, constantBuffers[CBTYPE_MISC], CB_GETBINDSLOT(MiscCB), threadID);
	device->BindConstantBuffer(PS, constantBuffers[CBTYPE_MISC], CB_GETBINDSLOT(MiscCB), threadID);
	device->BindConstantBuffer(GS, constantBuffers[CBTYPE_MISC], CB_GETBINDSLOT(MiscCB), threadID);
	device->BindConstantBuffer(DS, constantBuffers[CBTYPE_MISC], CB_GETBINDSLOT(MiscCB), threadID);
	device->BindConstantBuffer(HS, constantBuffers[CBTYPE_MISC], CB_GETBINDSLOT(MiscCB), threadID);
	device->BindConstantBuffer(CS, constantBuffers[CBTYPE_MISC], CB_GETBINDSLOT(MiscCB), threadID);

	device->BindConstantBuffer(VS, constantBuffers[CBTYPE_API], CB_GETBINDSLOT(APICB), threadID);
	device->BindConstantBuffer(PS, constantBuffers[CBTYPE_API], CB_GETBINDSLOT(APICB), threadID);
}

void wiRenderer::FixedUpdate()
{
}
void wiRenderer::UpdatePerFrameData(float dt)
{
	Scene& scene = GetScene();

	scene.Update(dt * GetGameSpeed());

	*getPrevCamera() = *getCamera();
	*getCamera() = *scene.cameras.GetComponent(cameraID);

	// Update Voxelization parameters:
	if (scene.objects.GetCount() > 0)
	{
		// We don't update it if the scene is empty, this even makes it easier to debug
		const float f = 0.05f / voxelSceneData.voxelsize;
		XMFLOAT3 center = XMFLOAT3(floorf(getCamera()->Eye.x * f) / f, floorf(getCamera()->Eye.y * f) / f, floorf(getCamera()->Eye.z * f) / f);
		if (wiMath::DistanceSquared(center, voxelSceneData.center) > 0)
		{
			voxelSceneData.centerChangedThisFrame = true;
		}
		else
		{
			voxelSceneData.centerChangedThisFrame = false;
		}
		voxelSceneData.center = center;
		voxelSceneData.extents = XMFLOAT3(voxelSceneData.res * voxelSceneData.voxelsize, voxelSceneData.res * voxelSceneData.voxelsize, voxelSceneData.res * voxelSceneData.voxelsize);
	}

	// Perform culling and obtain closest reflector:
	requestReflectionRendering = false;
	wiProfiler::GetInstance().BeginRange("Frustum Culling", wiProfiler::DOMAIN_CPU);
	{
		for (auto& x : frameCullings)
		{
			CameraComponent* camera = x.first;
			FrameCulling& culling = x.second;
			culling.Clear();

			if (!freezeCullingCamera)
			{
				culling.frustum = camera->frustum;
			}

			// Cull objects for every camera:
			for (size_t i = 0; i < scene.aabb_objects.GetCount(); ++i)
			{
				const AABB& aabb = scene.aabb_objects[i];

				if (culling.frustum.CheckBox(aabb))
				{
					const ObjectComponent& object = scene.objects[i];
					Entity entity = scene.objects.GetEntity(i);

					culling.culledObjects.push_back(entity);
					if (object.GetRenderTypes() & RENDERTYPE_OPAQUE)
					{
						culling.culledRenderer_opaque[object.meshID].push_back(entity);
					}
					if (object.GetRenderTypes() & RENDERTYPE_TRANSPARENT)
					{
						culling.culledRenderer_transparent[object.meshID].push_back(entity);
					}
					if (object.GetRenderTypes() & RENDERTYPE_WATER)
					{
						requestReflectionRendering = true;
					}
				}
			}

			// the following cullings will be only for the main camera:
			if (camera == getCamera())
			{
				// Cull decals:
				for (size_t i = 0; i < scene.aabb_decals.GetCount(); ++i)
				{
					const AABB& aabb = scene.aabb_decals[i];

					if (culling.frustum.CheckBox(aabb))
					{
						Entity entity = scene.decals.GetEntity(i);
						culling.culledDecals.push_back(entity);
					}
				}

				// Cull probes:
				for (size_t i = 0; i < scene.aabb_probes.GetCount(); ++i)
				{
					const AABB& aabb = scene.aabb_probes[i];

					if (culling.frustum.CheckBox(aabb))
					{
						Entity entity = scene.probes.GetEntity(i);
						culling.culledEnvProbes.push_back(entity);
					}
				}

				// Cull lights:
				for (size_t i = 0; i < scene.aabb_lights.GetCount(); ++i)
				{
					const AABB& aabb = scene.aabb_lights[i];

					if (culling.frustum.CheckBox(aabb))
					{
						Entity entity = scene.lights.GetEntity(i);
						culling.culledLights.push_back(entity);
					}
				}

				int i = 0;
				int shadowCounter_2D = 0;
				int shadowCounter_Cube = 0;
				for (Entity entity : culling.culledLights)
				{
					LightComponent& light = *scene.lights.GetComponent(entity);
					light.entityArray_index = i;

					// Link shadowmaps to lights till there are free slots

					light.shadowMap_index = -1;

					if (light.shadow)
					{
						switch (light.GetType())
						{
						case LightComponent::DIRECTIONAL:
							if (!light.shadowCam_dirLight.empty() && (shadowCounter_2D + 2) < SHADOWCOUNT_2D)
							{
								light.shadowMap_index = shadowCounter_2D;
								shadowCounter_2D += 3;
							}
							break;
						case LightComponent::SPOT:
							if (!light.shadowCam_spotLight.empty() && shadowCounter_2D < SHADOWCOUNT_2D)
							{
								light.shadowMap_index = shadowCounter_2D;
								shadowCounter_2D++;
							}
							break;
						case LightComponent::POINT:
						case LightComponent::SPHERE:
						case LightComponent::DISC:
						case LightComponent::RECTANGLE:
						case LightComponent::TUBE:
							if (!light.shadowCam_pointLight.empty() && shadowCounter_Cube < SHADOWCOUNT_CUBE)
							{
								light.shadowMap_index = shadowCounter_Cube;
								shadowCounter_Cube++;
							}
							break;
						default:
							break;
						}
					}

					i++;
				}

			}

		}
	}
	wiProfiler::GetInstance().EndRange(); // Frustum Culling

	// Ocean will override any current reflectors
	waterPlane = scene.waterPlane;
	if (ocean != nullptr)
	{
		requestReflectionRendering = true; 
		XMVECTOR _refPlane = XMPlaneFromPointNormal(XMVectorSet(0, ocean->waterHeight, 0, 0), XMVectorSet(0, 1, 0, 0));
		XMStoreFloat4(&waterPlane, _refPlane);
	}


	if (GetTemporalAAEnabled())
	{
		const XMFLOAT4& halton = wiMath::GetHaltonSequence(GetDevice()->GetFrameCount() % 256);
		static float jitter = 1.0f;
		temporalAAJitterPrev = temporalAAJitter;
		temporalAAJitter.x = jitter * (halton.x * 2 - 1) / (float)GetInternalResolution().x;
		temporalAAJitter.y = jitter * (halton.y * 2 - 1) / (float)GetInternalResolution().y;
		getCamera()->Projection.m[2][0] = temporalAAJitter.x;
		getCamera()->Projection.m[2][1] = temporalAAJitter.y;
		getCamera()->UpdateCamera();
	}
	else
	{
		temporalAAJitter = XMFLOAT2(0, 0);
		temporalAAJitterPrev = XMFLOAT2(0, 0);
	}

	*getRefCamera() = *getCamera();
	getRefCamera()->Reflect(waterPlane);

	for (auto& x : waterRipples)
	{
		x->Update(dt * 60 * GetGameSpeed());
	}

	renderTime_Prev = renderTime;
	renderTime += dt * GetGameSpeed();
	deltaTime = dt;
}
void wiRenderer::UpdateRenderData(GRAPHICSTHREAD threadID)
{
	GraphicsDevice* device = GetDevice();
	Scene& scene = GetScene();

	// Process deferred MIP generation:
	deferredMIPGenLock.lock();
	for (auto& it : deferredMIPGens)
	{
		GenerateMipChain(it, MIPGENFILTER_LINEAR, threadID);
	}
	deferredMIPGens.clear();
	deferredMIPGenLock.unlock();

	// Update material constant buffers:
	MaterialCB materialGPUData;
	for (size_t i = 0; i < scene.materials.GetCount(); ++i)
	{
		MaterialComponent& material = scene.materials[i];

		if (material.dirty)
		{
			material.dirty = false;

			materialGPUData.Create(material);

			if (material.constantBuffer == nullptr)
			{
				GPUBufferDesc desc;
				desc.Usage = USAGE_DEFAULT;
				desc.BindFlags = BIND_CONSTANT_BUFFER;
				desc.ByteWidth = sizeof(MaterialCB);

				SubresourceData InitData;
				InitData.pSysMem = &materialGPUData;

				material.constantBuffer.reset(new GPUBuffer);
				device->CreateBuffer(&desc, &InitData, material.constantBuffer.get());
			}
			else
			{
				device->UpdateBuffer(material.constantBuffer.get(), &materialGPUData, threadID);
			}

		}

	}


	const FrameCulling& mainCameraCulling = frameCullings[getCamera()];

	// Fill Light Array with lights + envprobes + decals in the frustum:
	{
		const auto& culledLights = mainCameraCulling.culledLights;

		static ShaderEntityType* entityArray = (ShaderEntityType*)_mm_malloc(sizeof(ShaderEntityType)*MAX_SHADER_ENTITY_COUNT, 16);
		static XMMATRIX* matrixArray = (XMMATRIX*)_mm_malloc(sizeof(XMMATRIX)*MATRIXARRAY_COUNT, 16);

		const XMMATRIX viewMatrix = getCamera()->GetView();

		UINT entityCounter = 0;
		UINT matrixCounter = 0;

		entityArrayOffset_Lights = 0;
		entityArrayCount_Lights = 0;
		entityArrayOffset_Decals = 0;
		entityArrayCount_Decals = 0;
		entityArrayOffset_ForceFields = 0;
		entityArrayCount_ForceFields = 0;
		entityArrayOffset_EnvProbes = 0;
		entityArrayCount_EnvProbes = 0;

		entityArrayOffset_Lights = entityCounter;
		for (Entity entity : culledLights)
		{
			if (entityCounter == MAX_SHADER_ENTITY_COUNT)
			{
				assert(0); // too many entities!
				entityCounter--;
				break;
			}

			const LightComponent& light = *scene.lights.GetComponent(entity);

			const int shadowIndex = light.shadowMap_index;

			entityArray[entityCounter].type = light.GetType();
			entityArray[entityCounter].positionWS = light.position;
			XMStoreFloat3(&entityArray[entityCounter].positionVS, XMVector3TransformCoord(XMLoadFloat3(&entityArray[entityCounter].positionWS), viewMatrix));
			entityArray[entityCounter].range = light.range;
			entityArray[entityCounter].color = wiMath::CompressColor(light.color);
			entityArray[entityCounter].energy = light.energy;
			entityArray[entityCounter].shadowBias = light.shadowBias;
			entityArray[entityCounter].additionalData_index = shadowIndex;
			switch (light.GetType())
			{
			case LightComponent::DIRECTIONAL:
			{
				entityArray[entityCounter].directionWS = light.direction;
				entityArray[entityCounter].shadowKernel = 1.0f / SHADOWRES_2D;

				if (light.shadow && shadowIndex >= 0 && !light.shadowCam_dirLight.empty())
				{
					matrixArray[shadowIndex + 0] = light.shadowCam_dirLight[0].getVP();
					matrixArray[shadowIndex + 1] = light.shadowCam_dirLight[1].getVP();
					matrixArray[shadowIndex + 2] = light.shadowCam_dirLight[2].getVP();
					matrixCounter = max(matrixCounter, (UINT)shadowIndex + 3);
				}
			}
			break;
			case LightComponent::SPOT:
			{
				entityArray[entityCounter].coneAngleCos = cosf(light.fov * 0.5f);
				entityArray[entityCounter].directionWS = light.direction;
				XMStoreFloat3(&entityArray[entityCounter].directionVS, XMVector3TransformNormal(XMLoadFloat3(&entityArray[entityCounter].directionWS), viewMatrix));
				entityArray[entityCounter].shadowKernel = 1.0f / SHADOWRES_2D;

				if (light.shadow && shadowIndex >= 0 && !light.shadowCam_spotLight.empty())
				{
					matrixArray[shadowIndex + 0] = light.shadowCam_spotLight[0].getVP();
					matrixCounter = max(matrixCounter, (UINT)shadowIndex + 1);
				}
			}
			break;
			case LightComponent::POINT:
			{
				entityArray[entityCounter].shadowKernel = 1.0f / SHADOWRES_CUBE;
			}
			break;
			case LightComponent::SPHERE:
			case LightComponent::DISC:
			case LightComponent::RECTANGLE:
			case LightComponent::TUBE:
			{
				// Note: area lights are facing back by default
				entityArray[entityCounter].directionWS = light.right;
				entityArray[entityCounter].directionVS = light.direction;
				entityArray[entityCounter].positionVS = light.front;
				entityArray[entityCounter].texMulAdd = XMFLOAT4(light.radius, light.width, light.height, 0);
			}
			break;
			}

			entityCounter++;
		}
		entityArrayCount_Lights = entityCounter - entityArrayOffset_Lights;

		entityArrayOffset_EnvProbes = entityCounter;
		for (Entity entity : mainCameraCulling.culledEnvProbes)
		{
			if (entityCounter == MAX_SHADER_ENTITY_COUNT)
			{
				assert(0); // too many entities!
				entityCounter--;
				break;
			}
			if (matrixCounter >= MATRIXARRAY_COUNT)
			{
				assert(0); // too many probes, can't upload the rest to matrixarray!
				matrixCounter--;
				break;
			}

			EnvironmentProbeComponent& probe = *scene.probes.GetComponent(entity);
			if (probe.textureIndex < 0)
			{
				continue;
			}

			entityArray[entityCounter].type = ENTITY_TYPE_ENVMAP;
			entityArray[entityCounter].positionWS = probe.position;
			XMStoreFloat3(&entityArray[entityCounter].positionVS, XMVector3TransformCoord(XMLoadFloat3(&probe.position), viewMatrix));
			entityArray[entityCounter].range = probe.range;
			entityArray[entityCounter].shadowBias = (float)probe.textureIndex;

			entityArray[entityCounter].additionalData_index = matrixCounter;
			matrixArray[matrixCounter] = XMMatrixTranspose(XMLoadFloat4x4(&probe.inverseMatrix));
			matrixCounter++;

			entityCounter++;
		}
		entityArrayCount_EnvProbes = entityCounter - entityArrayOffset_EnvProbes;

		entityArrayOffset_Decals = entityCounter;
		for (Entity entity : mainCameraCulling.culledDecals)
		{
			if (entityCounter == MAX_SHADER_ENTITY_COUNT)
			{
				assert(0); // too many entities!
				entityCounter--;
				break;
			}
			if (matrixCounter >= MATRIXARRAY_COUNT)
			{
				assert(0); // too many decals, can't upload the rest to matrixarray!
				matrixCounter--;
				break;
			}
			const DecalComponent& decal = *scene.decals.GetComponent(entity);

			entityArray[entityCounter].type = ENTITY_TYPE_DECAL;
			entityArray[entityCounter].positionWS = decal.position;
			XMStoreFloat3(&entityArray[entityCounter].positionVS, XMVector3TransformCoord(XMLoadFloat3(&decal.position), viewMatrix));
			entityArray[entityCounter].range = decal.range;
			entityArray[entityCounter].texMulAdd = decal.atlasMulAdd;
			entityArray[entityCounter].color = wiMath::CompressColor(XMFLOAT4(decal.color.x, decal.color.y, decal.color.z, decal.GetOpacity()));
			entityArray[entityCounter].energy = decal.emissive;

			entityArray[entityCounter].additionalData_index = matrixCounter;
			matrixArray[matrixCounter] = XMMatrixTranspose(XMMatrixInverse(nullptr, XMLoadFloat4x4(&decal.world)));
			matrixCounter++;

			entityCounter++;
		}
		entityArrayCount_Decals = entityCounter - entityArrayOffset_Decals;

		entityArrayOffset_ForceFields = entityCounter;
		for (size_t i = 0; i < scene.forces.GetCount(); ++i)
		{
			if (entityCounter == MAX_SHADER_ENTITY_COUNT)
			{
				assert(0); // too many entities!
				entityCounter--;
				break;
			}

			const ForceFieldComponent& force = scene.forces[i];
			Entity entity = scene.forces.GetEntity(i);

			entityArray[entityCounter].type = force.type;
			entityArray[entityCounter].positionWS = force.position;
			entityArray[entityCounter].energy = force.gravity;
			entityArray[entityCounter].range = 1.0f / max(0.0001f, force.range); // avoid division in shader
			entityArray[entityCounter].coneAngleCos = force.range; // this will be the real range in the less common shaders...
			// The default planar force field is facing upwards, and thus the pull direction is downwards:
			entityArray[entityCounter].directionWS = force.direction;

			entityCounter++;
		}
		entityArrayCount_ForceFields = entityCounter - entityArrayOffset_ForceFields;

		device->UpdateBuffer(resourceBuffers[RBTYPE_ENTITYARRAY], entityArray, threadID, sizeof(ShaderEntityType)*entityCounter);
		device->UpdateBuffer(resourceBuffers[RBTYPE_MATRIXARRAY], matrixArray, threadID, sizeof(XMMATRIX)*matrixCounter);

		GPUResource* resources[] = {
			resourceBuffers[RBTYPE_ENTITYARRAY],
			resourceBuffers[RBTYPE_MATRIXARRAY],
		};
		device->BindResources(VS, resources, SBSLOT_ENTITYARRAY, ARRAYSIZE(resources), threadID);
		device->BindResources(PS, resources, SBSLOT_ENTITYARRAY, ARRAYSIZE(resources), threadID);
		device->BindResources(CS, resources, SBSLOT_ENTITYARRAY, ARRAYSIZE(resources), threadID);
	}


	UpdateWorldCB(threadID); // only commits when parameters are changed
	UpdateFrameCB(threadID);
	BindPersistentState(threadID);

	ManageDecalAtlas(threadID);

	wiProfiler::GetInstance().BeginRange("Skinning", wiProfiler::DOMAIN_GPU, threadID);
	device->EventBegin("Skinning", threadID);
	{
		bool streamOutSetUp = false;
		CSTYPES lastCS = CSTYPE_SKINNING_LDS;

		for (size_t i = 0; i < scene.meshes.GetCount(); ++i)
		{
			MeshComponent& mesh = scene.meshes[i];

			if (mesh.IsSkinned())
			{
				ArmatureComponent& armature = *scene.armatures.GetComponent(mesh.armatureID);

				if (armature.boneBuffer == nullptr)
				{
					armature.boneData.resize(armature.boneCollection.size());

					GPUBufferDesc bd;
					bd.Usage = USAGE_DYNAMIC;
					bd.CPUAccessFlags = CPU_ACCESS_WRITE;

					bd.ByteWidth = sizeof(ArmatureComponent::ShaderBoneType) * (UINT)armature.boneCollection.size();
					bd.BindFlags = BIND_SHADER_RESOURCE;
					bd.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
					bd.StructureByteStride = sizeof(ArmatureComponent::ShaderBoneType);

					armature.boneBuffer.reset(new GPUBuffer);
					HRESULT hr = device->CreateBuffer(&bd, nullptr, armature.boneBuffer.get());
					assert(SUCCEEDED(hr));
				}

				if (!streamOutSetUp)
				{
					// Set up skinning shader
					streamOutSetUp = true;
					GPUBuffer* vbs[] = {
						nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr
					};
					const UINT strides[] = {
						0,0,0,0,0,0,0,0
					};
					device->BindVertexBuffers(vbs, 0, ARRAYSIZE(vbs), strides, nullptr, threadID);
					device->BindComputePSO(CPSO[CSTYPE_SKINNING_LDS], threadID);
				}

				CSTYPES targetCS = CSTYPE_SKINNING_LDS;

				if (!GetLDSSkinningEnabled() || armature.skinningMatrices.size() > SKINNING_COMPUTE_THREADCOUNT)
				{
					// If we have more bones that can fit into LDS, we switch to a skinning shader which loads from device memory:
					targetCS = CSTYPE_SKINNING;
				}

				if (targetCS != lastCS)
				{
					lastCS = targetCS;
					device->BindComputePSO(CPSO[targetCS], threadID);
				}

				// Upload bones for skinning to shader
				for (size_t k = 0; k < armature.skinningMatrices.size(); k++)
				{
					armature.boneData[k].Create(armature.skinningMatrices[k]);
				}
				device->UpdateBuffer(armature.boneBuffer.get(), armature.boneData.data(), threadID, (int)(sizeof(ArmatureComponent::ShaderBoneType) * armature.boneData.size()));
				device->BindResource(CS, armature.boneBuffer.get(), SKINNINGSLOT_IN_BONEBUFFER, threadID);

				// Do the skinning
				GPUResource* vbs[] = {
					mesh.vertexBuffer_POS.get(),
					mesh.vertexBuffer_BON.get(),
				};
				GPUResource* sos[] = {
					mesh.streamoutBuffer_POS.get(),
					mesh.streamoutBuffer_PRE.get(),
				};

				device->BindResources(CS, vbs, SKINNINGSLOT_IN_VERTEX_POS, ARRAYSIZE(vbs), threadID);
				device->BindUAVs(CS, sos, 0, ARRAYSIZE(sos), threadID);

				device->Dispatch((UINT)ceilf((float)mesh.vertex_positions.size() / SKINNING_COMPUTE_THREADCOUNT), 1, 1, threadID);
				device->UAVBarrier(sos, ARRAYSIZE(sos), threadID); // todo: defer, to gain from async compute
			}

		}

		if (streamOutSetUp)
		{
			device->UnbindUAVs(0, 2, threadID);
			device->UnbindResources(SKINNINGSLOT_IN_VERTEX_POS, 2, threadID);
		}

	}
	device->EventEnd(threadID);
	wiProfiler::GetInstance().EndRange(threadID); // skinning

	// GPU Particle systems simulation/sorting/culling:
	for (size_t i = 0; i < scene.emitters.GetCount(); ++i)
	{
		wiEmittedParticle& emitter = scene.emitters[i];
		Entity entity = scene.emitters.GetEntity(i);
		const TransformComponent& transform = *scene.transforms.GetComponent(entity);
		const MaterialComponent& material = *scene.materials.GetComponent(entity);
		const MeshComponent* mesh = scene.meshes.GetComponent(emitter.meshID);

		emitter.UpdateRenderData(transform, material, mesh, threadID);
	}

	// Hair particle systems simulation:
	for (size_t i = 0; i < scene.hairs.GetCount(); ++i)
	{
		wiHairParticle& hair = scene.hairs[i];

		if (hair.meshID != INVALID_ENTITY && getCamera()->frustum.CheckBox(hair.aabb))
		{
			const MeshComponent* mesh = scene.meshes.GetComponent(hair.meshID);

			if (mesh != nullptr)
			{
				Entity entity = scene.hairs.GetEntity(i);
				const MaterialComponent& material = *scene.materials.GetComponent(entity);

				hair.UpdateRenderData(*mesh, material, threadID);
			}
		}
	}

	// Compute water simulation:
	if (ocean != nullptr)
	{
		ocean->UpdateDisplacementMap(renderTime, threadID);
	}

	// Generate cloud layer:
	if(enviroMap == nullptr && GetScene().cloudiness > 0) // generate only when sky is dynamic
	{
		if (textures[TEXTYPE_2D_CLOUDS] == nullptr)
		{
			TextureDesc desc;
			desc.ArraySize = 1;
			desc.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = 0;
			desc.Format = FORMAT_R8G8B8A8_UNORM;
			desc.Height = 128;
			desc.Width = 128;
			desc.MipLevels = 1;
			desc.MiscFlags = 0;
			desc.Usage = USAGE_DEFAULT;

			device->CreateTexture2D(&desc, nullptr, (Texture2D**)&textures[TEXTYPE_2D_CLOUDS]);
		}

		float cloudPhase = renderTime * GetScene().cloudSpeed;
		GenerateClouds((Texture2D*)textures[TEXTYPE_2D_CLOUDS], 5, cloudPhase, GRAPHICSTHREAD_IMMEDIATE);
	}

	// Render out of date environment probes:
	RefreshEnvProbes(threadID);
}
void wiRenderer::OcclusionCulling_Render(GRAPHICSTHREAD threadID)
{
	if (!GetOcclusionCullingEnabled() || GetFreezeCullingCameraEnabled())
	{
		return;
	}

	const FrameCulling& culling = frameCullings[getCamera()];

	wiProfiler::GetInstance().BeginRange("Occlusion Culling Render", wiProfiler::DOMAIN_GPU, threadID);

	int queryID = 0;

	if (!culling.culledObjects.empty())
	{
		GetDevice()->EventBegin("Occlusion Culling Render", threadID);

		GetDevice()->BindGraphicsPSO(PSO_occlusionquery, threadID);

		Scene& scene = GetScene();

		int queryID = 0;

		MiscCB cb;

		for (Entity entity : culling.culledObjects)
		{
			ObjectComponent& object = *scene.objects.GetComponent(entity);
			if (!object.renderable)
			{
				continue;
			}

			if (queryID >= ARRAYSIZE(occlusionQueries))
			{
				object.occlusionQueryID = -1; // assign an invalid id from the pool
				continue;
			}

			// If a query could be retrieved from the pool for the instance, the instance can be occluded, so render it
			GPUQuery& query = occlusionQueries[queryID];
			if (!query.IsValid())
			{
				continue;
			}

			const AABB& aabb = *scene.aabb_objects.GetComponent(entity);

			if (aabb.intersects(getCamera()->Eye))
			{
				// camera is inside the instance, mark it as visible in this frame:
				object.occlusionHistory |= 1;
			}
			else
			{
				// only query for occlusion if the camera is outside the instance
				object.occlusionQueryID = queryID; // just assign the id from the pool
				queryID++;

				// previous frame view*projection because these are drawn against the previous depth buffer:
				cb.mTransform = XMMatrixTranspose(aabb.getAsBoxMatrix()*getPrevCamera()->GetViewProjection()); // todo: obb
				GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_MISC], &cb, threadID);

				// render bounding box to later read the occlusion status
				GetDevice()->QueryBegin(&query, threadID);
				GetDevice()->Draw(14, 0, threadID);
				GetDevice()->QueryEnd(&query, threadID);
			}
		}

		GetDevice()->EventEnd(threadID);
	}

	wiProfiler::GetInstance().EndRange(threadID); // Occlusion Culling Render
}
void wiRenderer::OcclusionCulling_Read()
{
	if (!GetOcclusionCullingEnabled() || GetFreezeCullingCameraEnabled())
	{
		return;
	}

	wiProfiler::GetInstance().BeginRange("Occlusion Culling Read", wiProfiler::DOMAIN_CPU);

	const FrameCulling& culling = frameCullings[getCamera()];

	if (!culling.culledObjects.empty())
	{
		GetDevice()->EventBegin("Occlusion Culling Read", GRAPHICSTHREAD_IMMEDIATE);

		Scene& scene = GetScene();

		for (Entity entity : culling.culledObjects)
		{
			ObjectComponent& object = *scene.objects.GetComponent(entity);
			if (!object.renderable)
			{
				continue;
			}

			object.occlusionHistory <<= 1; // advance history by 1 frame
			if (object.occlusionQueryID < 0)
			{
				object.occlusionHistory |= 1; // mark this frame as visible
				continue;
			}
			GPUQuery& query = occlusionQueries[object.occlusionQueryID];
			if (!query.IsValid())
			{
				object.occlusionHistory |= 1; // mark this frame as visible
				continue;
			}

			while (!GetDevice()->QueryRead(&query, GRAPHICSTHREAD_IMMEDIATE)) {}

			if (query.result_passed == TRUE)
			{
				object.occlusionHistory |= 1; // mark this frame as visible
			}
			else
			{
				// leave this frame as occluded
			}
		}

		GetDevice()->EventEnd(GRAPHICSTHREAD_IMMEDIATE);
	}

	wiProfiler::GetInstance().EndRange(); // Occlusion Culling Read
}
void wiRenderer::PutWaterRipple(const std::string& image, const XMFLOAT3& pos)
{
	wiSprite* img=new wiSprite("","",image);
	img->anim.fad=0.01f;
	img->anim.scaleX=0.2f;
	img->anim.scaleY=0.2f;
	img->effects.pos=pos;
	img->effects.rotation=(wiRandom::getRandom(0,1000)*0.001f)*2*3.1415f;
	img->effects.siz=XMFLOAT2(1,1);
	img->effects.typeFlag=WORLD;
	img->effects.quality=QUALITY_ANISOTROPIC;
	img->effects.pivot = XMFLOAT2(0.5f, 0.5f);
	img->effects.lookAt=waterPlane;
	img->effects.lookAt.w=1;
	waterRipples.push_back(img);
}
void wiRenderer::ManageWaterRipples(){
	while(	
		!waterRipples.empty() && 
			(waterRipples.front()->effects.opacity <= 0 + FLT_EPSILON || waterRipples.front()->effects.fade==1)
		)
		waterRipples.pop_front();
}
void wiRenderer::DrawWaterRipples(GRAPHICSTHREAD threadID)
{
	GetDevice()->EventBegin("Water Ripples", threadID);
	for(wiSprite* i:waterRipples){
		i->DrawNormal(threadID);
	}
	GetDevice()->EventEnd(threadID);
}

void wiRenderer::DrawDebugWorld(CameraComponent* camera, GRAPHICSTHREAD threadID)
{
	GraphicsDevice* device = GetDevice();

	Scene& scene = GetScene();

	device->EventBegin("DrawDebugWorld", threadID);

	if (debugBoneLines)
	{
		device->EventBegin("DebugBoneLines", threadID);

		device->BindGraphicsPSO(PSO_debug[DEBUGRENDERING_LINES], threadID);

		MiscCB sb;
		sb.mTransform = XMMatrixTranspose(camera->GetViewProjection());
		sb.mColor = XMFLOAT4(1, 1, 1, 1);
		device->UpdateBuffer(constantBuffers[CBTYPE_MISC], &sb, threadID);

		for (size_t i = 0; i < scene.armatures.GetCount(); ++i)
		{
			const ArmatureComponent& armature = scene.armatures[i];

			if (armature.boneCollection.empty())
			{
				continue;
			}

			struct LineSegment
			{
				XMFLOAT4 a, colorA, b, colorB;
			};
			UINT offset;
			void* mem = device->AllocateFromRingBuffer(dynamicVertexBufferPool, sizeof(LineSegment) * armature.boneCollection.size(), offset, threadID);

			int j = 0;
			for (Entity entity : armature.boneCollection)
			{
				const TransformComponent& transform = *scene.transforms.GetComponent(entity);

				XMMATRIX world = XMLoadFloat4x4(&transform.world);
				XMVECTOR a = XMVectorSet(0, 0, 0, 1);
				XMVECTOR b = XMVectorSet(0, 0, 1, 1);

				a = XMVector4Transform(a, world);
				b = XMVector4Transform(b, world);


				LineSegment segment;
				XMStoreFloat4(&segment.a, a);
				XMStoreFloat4(&segment.b, b);

				memcpy((void*)((size_t)mem + j * sizeof(LineSegment)), &segment, sizeof(LineSegment));
				j++;
			}

			device->InvalidateBufferAccess(dynamicVertexBufferPool, threadID);

			GPUBuffer* vbs[] = {
				dynamicVertexBufferPool,
			};
			const UINT strides[] = {
				sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
			};
			const UINT offsets[] = {
				offset,
			};
			device->BindVertexBuffers(vbs, 0, ARRAYSIZE(vbs), strides, offsets, threadID);

			device->Draw(2 * j, 0, threadID);

		}

		device->EventEnd(threadID);
	}
	
	if (!renderableLines.empty())
	{
		device->EventBegin("DebugLines", threadID);

		device->BindGraphicsPSO(PSO_debug[DEBUGRENDERING_LINES], threadID);

		MiscCB sb;
		sb.mTransform = XMMatrixTranspose(camera->GetViewProjection());
		sb.mColor = XMFLOAT4(1, 1, 1, 1);
		device->UpdateBuffer(constantBuffers[CBTYPE_MISC], &sb, threadID);

		struct LineSegment
		{
			XMFLOAT4 a, colorA, b, colorB;
		};
		UINT offset;
		void* mem = device->AllocateFromRingBuffer(dynamicVertexBufferPool, sizeof(LineSegment) * renderableLines.size(), offset, threadID);

		int i = 0;
		for (auto& line : renderableLines)
		{
			LineSegment segment;
			segment.a = XMFLOAT4(line.start.x, line.start.y, line.start.z, 1);
			segment.b = XMFLOAT4(line.end.x, line.end.y, line.end.z, 1);
			segment.colorA = segment.colorB = line.color;

			memcpy((void*)((size_t)mem + i * sizeof(LineSegment)), &segment, sizeof(LineSegment));
			i++;
		}

		device->InvalidateBufferAccess(dynamicVertexBufferPool, threadID);

		GPUBuffer* vbs[] = {
			dynamicVertexBufferPool,
		};
		const UINT strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		const UINT offsets[] = {
			offset,
		};
		device->BindVertexBuffers(vbs, 0, ARRAYSIZE(vbs), strides, offsets, threadID);

		device->Draw(2 * i, 0, threadID);

		renderableLines.clear();

		device->EventEnd(threadID);
	}

	if (!renderableBoxes.empty())
	{
		device->EventBegin("DebugBoxes", threadID);

		device->BindGraphicsPSO(PSO_debug[DEBUGRENDERING_CUBE], threadID);

		GPUBuffer* vbs[] = {
			&Cube::vertexBuffer,
		};
		const UINT strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		device->BindVertexBuffers(vbs, 0, ARRAYSIZE(vbs), strides, nullptr, threadID);
		device->BindIndexBuffer(&Cube::indexBuffer, INDEXFORMAT_16BIT, 0, threadID);

		MiscCB sb;

		for (auto& x : renderableBoxes)
		{
			sb.mTransform = XMMatrixTranspose(XMLoadFloat4x4(&x.first)*camera->GetViewProjection());
			sb.mColor = x.second;

			device->UpdateBuffer(constantBuffers[CBTYPE_MISC], &sb, threadID);

			device->DrawIndexed(24, 0, 0, threadID);
		}
		renderableBoxes.clear();

		device->EventEnd(threadID);
	}


	if (debugEnvProbes)
	{
		device->EventBegin("Debug EnvProbes", threadID);
		// Envmap spheres:

		device->BindGraphicsPSO(PSO_debug[DEBUGRENDERING_ENVPROBE], threadID);

		MiscCB sb;
		for (size_t i = 0; i < scene.probes.GetCount(); ++i)
		{
			EnvironmentProbeComponent& probe = scene.probes[i];

			sb.mTransform = XMMatrixTranspose(XMMatrixTranslationFromVector(XMLoadFloat3(&probe.position)));
			device->UpdateBuffer(constantBuffers[CBTYPE_MISC], &sb, threadID);

			if (probe.textureIndex < 0)
			{
				device->BindResource(PS, wiTextureHelper::getInstance()->getBlackCubeMap(), TEXSLOT_ONDEMAND0, threadID);
			}
			else
			{
				device->BindResource(PS, textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY], TEXSLOT_ONDEMAND0, threadID, textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY]->GetDesc().MipLevels + probe.textureIndex);
			}

			device->Draw(2880, 0, threadID); // uv-sphere
		}


		// Local proxy boxes:

		device->BindGraphicsPSO(PSO_debug[DEBUGRENDERING_CUBE], threadID);

		GPUBuffer* vbs[] = {
			&Cube::vertexBuffer,
		};
		const UINT strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		device->BindVertexBuffers(vbs, 0, ARRAYSIZE(vbs), strides, nullptr, threadID);
		device->BindIndexBuffer(&Cube::indexBuffer, INDEXFORMAT_16BIT, 0, threadID);

		for (size_t i = 0; i < scene.probes.GetCount(); ++i)
		{
			EnvironmentProbeComponent& probe = scene.probes[i];

			if (probe.textureIndex < 0)
			{
				continue;
			}

			Entity entity = scene.probes.GetEntity(i);
			const TransformComponent& transform = *scene.transforms.GetComponent(entity);

			sb.mTransform = XMMatrixTranspose(XMLoadFloat4x4(&transform.world)*camera->GetViewProjection());
			sb.mColor = XMFLOAT4(0, 1, 1, 1);

			device->UpdateBuffer(constantBuffers[CBTYPE_MISC], &sb, threadID);

			device->DrawIndexed(24, 0, 0, threadID);
		}

		device->EventEnd(threadID);
	}


	if (gridHelper)
	{
		device->EventBegin("GridHelper", threadID);

		device->BindGraphicsPSO(PSO_debug[DEBUGRENDERING_GRID], threadID);

		static float col = 0.7f;
		static int gridVertexCount = 0;
		static GPUBuffer* grid = nullptr;
		if (grid == nullptr)
		{
			const float h = 0.01f; // avoid z-fight with zero plane
			const int a = 20;
			XMFLOAT4 verts[((a + 1) * 2 + (a + 1) * 2) * 2];

			int count = 0;
			for (int i = 0; i <= a; ++i)
			{
				verts[count++] = XMFLOAT4(i - a * 0.5f, h, -a * 0.5f, 1);
				verts[count++] = (i == a / 2 ? XMFLOAT4(0, 0, 1, 1) : XMFLOAT4(col, col, col, 1));

				verts[count++] = XMFLOAT4(i - a * 0.5f, h, +a * 0.5f, 1);
				verts[count++] = (i == a / 2 ? XMFLOAT4(0, 0, 1, 1) : XMFLOAT4(col, col, col, 1));
			}
			for (int j = 0; j <= a; ++j)
			{
				verts[count++] = XMFLOAT4(-a * 0.5f, h, j - a * 0.5f, 1);
				verts[count++] = (j == a / 2 ? XMFLOAT4(1, 0, 0, 1) : XMFLOAT4(col, col, col, 1));

				verts[count++] = XMFLOAT4(+a * 0.5f, h, j - a * 0.5f, 1);
				verts[count++] = (j == a / 2 ? XMFLOAT4(1, 0, 0, 1) : XMFLOAT4(col, col, col, 1));
			}

			gridVertexCount = ARRAYSIZE(verts) / 2;

			GPUBufferDesc bd;
			ZeroMemory(&bd, sizeof(bd));
			bd.Usage = USAGE_IMMUTABLE;
			bd.ByteWidth = sizeof(verts);
			bd.BindFlags = BIND_VERTEX_BUFFER;
			bd.CPUAccessFlags = 0;
			SubresourceData InitData;
			ZeroMemory(&InitData, sizeof(InitData));
			InitData.pSysMem = verts;
			grid = new GPUBuffer;
			device->CreateBuffer(&bd, &InitData, grid);
		}

		MiscCB sb;
		sb.mTransform = XMMatrixTranspose(camera->GetViewProjection());
		sb.mColor = XMFLOAT4(1, 1, 1, 1);

		device->UpdateBuffer(constantBuffers[CBTYPE_MISC], &sb, threadID);

		GPUBuffer* vbs[] = {
			grid,
		};
		const UINT strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		device->BindVertexBuffers(vbs, 0, ARRAYSIZE(vbs), strides, nullptr, threadID);
		device->Draw(gridVertexCount, 0, threadID);

		device->EventEnd(threadID);
	}

	if (voxelHelper && textures[TEXTYPE_3D_VOXELRADIANCE] != nullptr)
	{
		device->EventBegin("Debug Voxels", threadID);

		device->BindGraphicsPSO(PSO_debug[DEBUGRENDERING_VOXEL], threadID);


		MiscCB sb;
		sb.mTransform = XMMatrixTranspose(XMMatrixTranslationFromVector(XMLoadFloat3(&voxelSceneData.center)) * camera->GetViewProjection());
		sb.mColor = XMFLOAT4(1, 1, 1, 1);

		device->UpdateBuffer(constantBuffers[CBTYPE_MISC], &sb, threadID);

		device->Draw(voxelSceneData.res * voxelSceneData.res * voxelSceneData.res, 0, threadID);

		device->EventEnd(threadID);
	}

	if (debugEmitters)
	{
		device->EventBegin("DebugEmitters", threadID);

		MiscCB sb;
		for (size_t i = 0; i < scene.emitters.GetCount(); ++i)
		{
			const wiEmittedParticle& emitter = scene.emitters[i];
			Entity entity = scene.emitters.GetEntity(i);
			const TransformComponent& transform = *scene.transforms.GetComponent(entity);
			const MeshComponent* mesh = scene.meshes.GetComponent(emitter.meshID);

			sb.mTransform = XMMatrixTranspose(XMLoadFloat4x4(&transform.world)*camera->GetViewProjection());
			sb.mColor = XMFLOAT4(0, 1, 0, 1);
			device->UpdateBuffer(constantBuffers[CBTYPE_MISC], &sb, threadID);

			if (mesh == nullptr)
			{
				// No mesh, just draw a box:
				device->BindGraphicsPSO(PSO_debug[DEBUGRENDERING_CUBE], threadID);
				GPUBuffer* vbs[] = {
					&Cube::vertexBuffer,
				};
				const UINT strides[] = {
					sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
				};
				device->BindVertexBuffers(vbs, 0, ARRAYSIZE(vbs), strides, nullptr, threadID);
				device->BindIndexBuffer(&Cube::indexBuffer, INDEXFORMAT_16BIT, 0, threadID);
				device->DrawIndexed(24, 0, 0, threadID);
			}
			else
			{
				// Draw mesh wireframe:
				device->BindGraphicsPSO(PSO_debug[DEBUGRENDERING_EMITTER], threadID);
				GPUBuffer* vbs[] = {
					mesh->streamoutBuffer_POS != nullptr ? mesh->streamoutBuffer_POS.get() : mesh->vertexBuffer_POS.get(),
				};
				const UINT strides[] = {
					sizeof(MeshComponent::Vertex_POS),
				};
				device->BindVertexBuffers(vbs, 0, ARRAYSIZE(vbs), strides, nullptr, threadID);
				device->BindIndexBuffer(mesh->indexBuffer.get(), mesh->GetIndexFormat(), 0, threadID);

				device->DrawIndexed((int)mesh->indices.size(), 0, 0, threadID);
			}
		}

		device->EventEnd(threadID);
	}


	if (debugForceFields)
	{
		device->EventBegin("DebugForceFields", threadID);

		MiscCB sb;
		for (size_t i = 0; i < scene.forces.GetCount(); ++i)
		{
			ForceFieldComponent& force = scene.forces[i];

			sb.mTransform = XMMatrixTranspose(camera->GetViewProjection());
			sb.mColor = XMFLOAT4(camera->Eye.x, camera->Eye.y, camera->Eye.z, (float)i);
			device->UpdateBuffer(constantBuffers[CBTYPE_MISC], &sb, threadID);

			switch (force.type)
			{
			case ENTITY_TYPE_FORCEFIELD_POINT:
				device->BindGraphicsPSO(PSO_debug[DEBUGRENDERING_FORCEFIELD_POINT], threadID);
				device->Draw(2880, 0, threadID); // uv-sphere
				break;
			case ENTITY_TYPE_FORCEFIELD_PLANE:
				device->BindGraphicsPSO(PSO_debug[DEBUGRENDERING_FORCEFIELD_PLANE], threadID);
				device->Draw(14, 0, threadID); // box
				break;
			}

			++i;
		}

		device->EventEnd(threadID);
	}


	if (debugCameras)
	{
		device->EventBegin("DebugCameras", threadID);

		device->BindGraphicsPSO(PSO_debug[DEBUGRENDERING_CUBE], threadID);

		GPUBuffer* vbs[] = {
			&Cube::vertexBuffer,
		};
		const UINT strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		device->BindVertexBuffers(vbs, 0, ARRAYSIZE(vbs), strides, nullptr, threadID);
		device->BindIndexBuffer(&Cube::indexBuffer, INDEXFORMAT_16BIT, 0, threadID);

		MiscCB sb;
		sb.mColor = XMFLOAT4(1, 1, 1, 1);

		for(size_t i = 0; i < scene.cameras.GetCount(); ++i)
		{
			CameraComponent& cam = scene.cameras[i];
			Entity entity = scene.cameras.GetEntity(i);

			if (entity == getCameraID()) // avoid drawing the main camera...
			{
				continue;
			}

			sb.mTransform = XMMatrixTranspose(cam.GetInvView()*camera->GetViewProjection());

			device->UpdateBuffer(constantBuffers[CBTYPE_MISC], &sb, threadID);

			device->DrawIndexed(24, 0, 0, threadID);
		}

		device->EventEnd(threadID);
	}

	device->EventEnd(threadID);
}

void wiRenderer::DrawSoftParticles(CameraComponent* camera, bool distortion, GRAPHICSTHREAD threadID)
{
	Scene& scene = GetScene();

	for (size_t i = 0; i < scene.emitters.GetCount(); ++i)
	{
		wiEmittedParticle& emitter = scene.emitters[i];
		Entity entity = scene.emitters.GetEntity(i);
		const MaterialComponent& material = *scene.materials.GetComponent(entity);

		if (distortion && emitter.shaderType == wiEmittedParticle::SOFT_DISTORTION)
		{
			emitter.Draw(material, threadID);
		}
		else if (!distortion && (emitter.shaderType == wiEmittedParticle::SOFT || emitter.shaderType == wiEmittedParticle::SIMPLEST || IsWireRender()))
		{
			emitter.Draw(material, threadID);
		}
	}

}
void wiRenderer::DrawTrails(GRAPHICSTHREAD threadID, Texture2D* refracRes)
{
	//if (objectsWithTrails.empty())
	//{
	//	return;
	//}

	//GetDevice()->EventBegin("RibbonTrails", threadID);

	//GetDevice()->BindPrimitiveTopology(TRIANGLESTRIP,threadID);
	//GetDevice()->BindVertexLayout(vertexLayouts[VLTYPE_TRAIL],threadID);

	//GetDevice()->BindRasterizerState(wireRender?rasterizers[RSTYPE_WIRE_DOUBLESIDED]:rasterizers[RSTYPE_DOUBLESIDED],threadID);
	//GetDevice()->BindDepthStencilState(depthStencils[DSSTYPE_DEFAULT],STENCILREF_EMPTY,threadID);
	//GetDevice()->BindBlendState(blendStates[BSTYPE_OPAQUE],threadID);

	//GetDevice()->BindPS(pixelShaders[PSTYPE_TRAIL],threadID);
	//GetDevice()->BindVS(vertexShaders[VSTYPE_TRAIL],threadID);
	//
	//GetDevice()->BindResource(PS, refracRes,TEXSLOT_ONDEMAND0,threadID);

	//for (Object* o : objectsWithTrails)
	//{
	//	if (o->trail.size() >= 4)
	//	{

	//		GetDevice()->BindResource(PS, o->trailDistortTex, TEXSLOT_ONDEMAND1, threadID);
	//		GetDevice()->BindResource(PS, o->trailTex, TEXSLOT_ONDEMAND2, threadID);

	//		std::vector<RibbonVertex> trails;

	//		int bounds = (int)o->trail.size();
	//		trails.reserve(bounds * 10);
	//		int req = bounds - 3;
	//		for (int k = 0; k < req; k += 2)
	//		{
	//			static const float trailres = 10.f;
	//			for (float r = 0.0f; r <= 1.0f; r += 1.0f / trailres)
	//			{
	//				XMVECTOR point0 = XMVectorCatmullRom(
	//					XMLoadFloat3(&o->trail[k ? (k - 2) : 0].pos)
	//					, XMLoadFloat3(&o->trail[k].pos)
	//					, XMLoadFloat3(&o->trail[k + 2].pos)
	//					, XMLoadFloat3(&o->trail[k + 6 < bounds ? (k + 6) : (bounds - 2)].pos)
	//					, r
	//				),
	//					point1 = XMVectorCatmullRom(
	//						XMLoadFloat3(&o->trail[k ? (k - 1) : 1].pos)
	//						, XMLoadFloat3(&o->trail[k + 1].pos)
	//						, XMLoadFloat3(&o->trail[k + 3].pos)
	//						, XMLoadFloat3(&o->trail[k + 5 < bounds ? (k + 5) : (bounds - 1)].pos)
	//						, r
	//					);
	//				XMFLOAT3 xpoint0, xpoint1;
	//				XMStoreFloat3(&xpoint0, point0);
	//				XMStoreFloat3(&xpoint1, point1);
	//				trails.push_back(RibbonVertex(xpoint0
	//					, wiMath::Lerp(XMFLOAT2((float)k / (float)bounds, 0), XMFLOAT2((float)(k + 1) / (float)bounds, 0), r)
	//					, wiMath::Lerp(o->trail[k].col, o->trail[k + 2].col, r)
	//					, 1
	//				));
	//				trails.push_back(RibbonVertex(xpoint1
	//					, wiMath::Lerp(XMFLOAT2((float)k / (float)bounds, 1), XMFLOAT2((float)(k + 1) / (float)bounds, 1), r)
	//					, wiMath::Lerp(o->trail[k + 1].col, o->trail[k + 3].col, r)
	//					, 1
	//				));
	//			}
	//		}
	//		if (!trails.empty())
	//		{
	//			UINT trailOffset;
	//			void* buffer = GetDevice()->AllocateFromRingBuffer(dynamicVertexBufferPool, sizeof(RibbonVertex)*trails.size(), trailOffset, threadID);
	//			memcpy(buffer, trails.data(), sizeof(RibbonVertex)*trails.size());
	//			GetDevice()->InvalidateBufferAccess(dynamicVertexBufferPool, threadID);

	//			const GPUBuffer* vbs[] = {
	//				dynamicVertexBufferPool
	//			};
	//			const UINT strides[] = {
	//				sizeof(RibbonVertex)
	//			};
	//			const UINT offsets[] = {
	//				trailOffset
	//			};
	//			GetDevice()->BindVertexBuffers(vbs, 0, ARRAYSIZE(vbs), strides, offsets, threadID);
	//			GetDevice()->Draw((int)trails.size(), 0, threadID);

	//			trails.clear();
	//		}
	//	}
	//}

	//GetDevice()->EventEnd(threadID);
}
void wiRenderer::DrawLights(CameraComponent* camera, GRAPHICSTHREAD threadID)
{
	const FrameCulling& culling = frameCullings[camera];
	const auto& culledLights = culling.culledLights;

	Scene& scene = GetScene();

	GetDevice()->EventBegin("Light Render", threadID);
	wiProfiler::GetInstance().BeginRange("Light Render", wiProfiler::DOMAIN_GPU, threadID);

	// Environmental light (envmap + voxelGI) is always drawn
	{
		GetDevice()->BindGraphicsPSO(PSO_enviromentallight, threadID);
		GetDevice()->Draw(3, 0, threadID); // full screen triangle
	}

	for (int type = 0; type < LightComponent::LIGHTTYPE_COUNT; ++type)
	{
		GetDevice()->BindGraphicsPSO(PSO_deferredlight[type], threadID);

		for (Entity entity : culledLights)
		{
			const LightComponent& light = *scene.lights.GetComponent(entity);
			if (light.GetType() != type)
				continue;

			switch (type)
			{
			case LightComponent::DIRECTIONAL:
			case LightComponent::SPHERE:
			case LightComponent::DISC:
			case LightComponent::RECTANGLE:
			case LightComponent::TUBE:
				{
					MiscCB miscCb;
					miscCb.mColor.x = (float)light.entityArray_index;
					GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_MISC], &miscCb, threadID);

					GetDevice()->Draw(3, 0, threadID); // full screen triangle
				}
				break;
			case LightComponent::POINT:
				{
					MiscCB miscCb;
					miscCb.mColor.x = (float)light.entityArray_index;
					float sca = light.range + 1;
					miscCb.mTransform = XMMatrixTranspose(XMMatrixScaling(sca, sca, sca)*XMMatrixTranslationFromVector(XMLoadFloat3(&light.position)) * camera->GetViewProjection());
					GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_MISC], &miscCb, threadID);

					GetDevice()->Draw(240, 0, threadID); // icosphere
				}
				break;
			case LightComponent::SPOT:
				{
					MiscCB miscCb;
					miscCb.mColor.x = (float)light.entityArray_index;
					const float coneS = (const float)(light.fov / XM_PIDIV4);
					miscCb.mTransform = XMMatrixTranspose(
						XMMatrixScaling(coneS*light.range, light.range, coneS*light.range)*
						XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation))*
						XMMatrixTranslationFromVector(XMLoadFloat3(&light.position)) *
						camera->GetViewProjection()
					);
					GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_MISC], &miscCb, threadID);

					GetDevice()->Draw(192, 0, threadID); // cone
				}
				break;
			}
		}


	}

	wiProfiler::GetInstance().EndRange(threadID);
	GetDevice()->EventEnd(threadID);
}
void wiRenderer::DrawLightVisualizers(CameraComponent* camera, GRAPHICSTHREAD threadID)
{
	Scene& scene = GetScene();

	GetDevice()->EventBegin("Light Visualizer Render", threadID);

	GetDevice()->BindConstantBuffer(PS, constantBuffers[CBTYPE_VOLUMELIGHT], CB_GETBINDSLOT(VolumeLightCB), threadID);
	GetDevice()->BindConstantBuffer(VS, constantBuffers[CBTYPE_VOLUMELIGHT], CB_GETBINDSLOT(VolumeLightCB), threadID);

	XMMATRIX camrot = XMLoadFloat3x3(&camera->rotationMatrix);


	for (int type = LightComponent::POINT; type < LightComponent::LIGHTTYPE_COUNT; ++type)
	{
		GetDevice()->BindGraphicsPSO(PSO_lightvisualizer[type], threadID);

		for (size_t i = 0; i < scene.lights.GetCount(); ++i) 
		{
			LightComponent& light = scene.lights[i];

			if (light.GetType() == type && light.visualizer) 
			{

				VolumeLightCB lcb;
				lcb.col = XMFLOAT4(light.color.x, light.color.y, light.color.z, 1);
				lcb.enerdis = XMFLOAT4(light.energy, light.range, light.fov, light.energy);

				if (type == LightComponent::POINT) 
				{
					lcb.enerdis.w = light.range*light.energy*0.01f; // scale
					lcb.world = XMMatrixTranspose(
						XMMatrixScaling(lcb.enerdis.w, lcb.enerdis.w, lcb.enerdis.w)*
						camrot*
						XMMatrixTranslationFromVector(XMLoadFloat3(&light.position))
					);

					GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_VOLUMELIGHT], &lcb, threadID);

					GetDevice()->Draw(108, 0, threadID); // circle
				}
				else if(type == LightComponent::SPOT)
				{
					float coneS = (float)(light.fov / 0.7853981852531433);
					lcb.enerdis.w = light.range*light.energy*0.03f; // scale
					lcb.world = XMMatrixTranspose(
						XMMatrixScaling(coneS*lcb.enerdis.w, lcb.enerdis.w, coneS*lcb.enerdis.w)*
						XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation))*
						XMMatrixTranslationFromVector(XMLoadFloat3(&light.position))
					);

					GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_VOLUMELIGHT], &lcb, threadID);

					GetDevice()->Draw(192, 0, threadID); // cone
				}
				else if (type == LightComponent::SPHERE)
				{
					lcb.world = XMMatrixTranspose(
						XMMatrixScaling(light.radius, light.radius, light.radius)*
						XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation))*
						XMMatrixTranslationFromVector(XMLoadFloat3(&light.position))*
						camera->GetViewProjection()
					);

					GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_VOLUMELIGHT], &lcb, threadID);

					GetDevice()->Draw(2880, 0, threadID); // uv-sphere
				}
				else if (type == LightComponent::DISC)
				{
					lcb.world = XMMatrixTranspose(
						XMMatrixScaling(light.radius, light.radius, light.radius)*
						XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation))*
						XMMatrixTranslationFromVector(XMLoadFloat3(&light.position))*
						camera->GetViewProjection()
					);

					GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_VOLUMELIGHT], &lcb, threadID);

					GetDevice()->Draw(108, 0, threadID); // circle
				}
				else if (type == LightComponent::RECTANGLE)
				{
					lcb.world = XMMatrixTranspose(
						XMMatrixScaling(light.width * 0.5f, light.height * 0.5f, 0.5f)*
						XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation))*
						XMMatrixTranslationFromVector(XMLoadFloat3(&light.position))*
						camera->GetViewProjection()
					);

					GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_VOLUMELIGHT], &lcb, threadID);

					GetDevice()->Draw(6, 0, threadID); // quad
				}
				else if (type == LightComponent::TUBE)
				{
					lcb.world = XMMatrixTranspose(
						XMMatrixScaling(max(light.width * 0.5f, light.radius), light.radius, light.radius)*
						XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation))*
						XMMatrixTranslationFromVector(XMLoadFloat3(&light.position))*
						camera->GetViewProjection()
					);

					GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_VOLUMELIGHT], &lcb, threadID);

					GetDevice()->Draw(384, 0, threadID); // cylinder
				}
			}
		}

	}

	GetDevice()->EventEnd(threadID);


}
void wiRenderer::DrawVolumeLights(CameraComponent* camera, GRAPHICSTHREAD threadID)
{
	const FrameCulling& culling = frameCullings[camera];
	const auto& culledLights = culling.culledLights;

	if (!culledLights.empty())
	{
		GetDevice()->EventBegin("Volumetric Light Render", threadID);

		Scene& scene = GetScene();

		for (int type = 0; type < LightComponent::LIGHTTYPE_COUNT; ++type)
		{
			GraphicsPSO* pso = PSO_volumetriclight[type];

			if (pso == nullptr)
			{
				continue;
			}

			GetDevice()->BindGraphicsPSO(pso, threadID);

			for (Entity entity : culledLights)
			{
				const LightComponent& light = *scene.lights.GetComponent(entity);
				if (light.GetType() == type && light.volumetrics)
				{

					switch (type)
					{
					case LightComponent::DIRECTIONAL:
					case LightComponent::SPHERE:
					case LightComponent::DISC:
					case LightComponent::RECTANGLE:
					case LightComponent::TUBE:
					{
						MiscCB miscCb;
						miscCb.mColor.x = (float)light.entityArray_index;
						GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_MISC], &miscCb, threadID);

						GetDevice()->Draw(3, 0, threadID); // full screen triangle
					}
					break;
					case LightComponent::POINT:
					{
						MiscCB miscCb;
						miscCb.mColor.x = (float)light.entityArray_index;
						float sca = light.range + 1;
						miscCb.mTransform = XMMatrixTranspose(XMMatrixScaling(sca, sca, sca)*XMMatrixTranslationFromVector(XMLoadFloat3(&light.position)) * camera->GetViewProjection());
						GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_MISC], &miscCb, threadID);

						GetDevice()->Draw(240, 0, threadID); // icosphere
					}
					break;
					case LightComponent::SPOT:
					{
						MiscCB miscCb;
						miscCb.mColor.x = (float)light.entityArray_index;
						const float coneS = (const float)(light.fov / XM_PIDIV4);
						miscCb.mTransform = XMMatrixTranspose(
							XMMatrixScaling(coneS*light.range, light.range, coneS*light.range)*
							XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation))*
							XMMatrixTranslationFromVector(XMLoadFloat3(&light.position)) *
							camera->GetViewProjection()
						);
						GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_MISC], &miscCb, threadID);

						GetDevice()->Draw(192, 0, threadID); // cone
					}
					break;
					}

				}
			}

		}

		GetDevice()->EventEnd(threadID);
	}


}
void wiRenderer::DrawLensFlares(GRAPHICSTHREAD threadID)
{
	const FrameCulling& culling = frameCullings[getCamera()];
	const auto& culledLights = culling.culledLights;

	Scene& scene = GetScene();

	for(Entity entity : culledLights)
	{
		const LightComponent& light = *scene.lights.GetComponent(entity);

		if(!light.lensFlareRimTextures.empty())
		{
			XMVECTOR POS;

			if(light.GetType() ==LightComponent::POINT || light.GetType() ==LightComponent::SPOT)
			{
				POS = XMLoadFloat3(&light.position);
			}

			else{
				POS = XMVector3Normalize(
					-XMVector3Transform(XMVectorSet(0, -1, 0, 1), XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation)))
				) * 100000;
			}
			
			XMVECTOR flarePos = XMVector3Project(POS,0.f,0.f,(float)GetInternalResolution().x,(float)GetInternalResolution().y,0.0f,1.0f,getCamera()->GetRealProjection(),getCamera()->GetView(),XMMatrixIdentity());

			if( XMVectorGetX(XMVector3Dot( XMVectorSubtract(POS,getCamera()->GetEye()),getCamera()->GetAt() ))>0 )
				wiLensFlare::Draw(threadID,flarePos,light.lensFlareRimTextures);

		}

	}
}

void wiRenderer::SetShadowProps2D(int resolution, int count, int softShadowQuality)
{
	SHADOWRES_2D = resolution;
	SHADOWCOUNT_2D = count;
	SOFTSHADOWQUALITY_2D = softShadowQuality;

	SAFE_DELETE(shadowMapArray_2D);
	shadowMapArray_2D = new Texture2D;
	shadowMapArray_2D->RequestIndependentRenderTargetArraySlices(true);

	SAFE_DELETE(shadowMapArray_Transparent);
	shadowMapArray_Transparent = new Texture2D;
	shadowMapArray_Transparent->RequestIndependentRenderTargetArraySlices(true);

	TextureDesc desc;
	ZeroMemory(&desc, sizeof(desc));
	desc.Width = SHADOWRES_2D;
	desc.Height = SHADOWRES_2D;
	desc.MipLevels = 1;
	desc.ArraySize = SHADOWCOUNT_2D;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = USAGE_DEFAULT;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;

	desc.BindFlags = BIND_DEPTH_STENCIL | BIND_SHADER_RESOURCE;
	desc.Format = DSFormat_small_alias;
	GetDevice()->CreateTexture2D(&desc, nullptr, &shadowMapArray_2D);

	desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
	desc.Format = RTFormat_ldr;
	GetDevice()->CreateTexture2D(&desc, nullptr, &shadowMapArray_Transparent);
}
void wiRenderer::SetShadowPropsCube(int resolution, int count)
{
	SHADOWRES_CUBE = resolution;
	SHADOWCOUNT_CUBE = count;

	SAFE_DELETE(shadowMapArray_Cube);
	shadowMapArray_Cube = new Texture2D;
	shadowMapArray_Cube->RequestIndependentRenderTargetArraySlices(true);
	shadowMapArray_Cube->RequestIndependentRenderTargetCubemapFaces(false);

	TextureDesc desc;
	ZeroMemory(&desc, sizeof(desc));
	desc.Width = SHADOWRES_CUBE;
	desc.Height = SHADOWRES_CUBE;
	desc.MipLevels = 1;
	desc.ArraySize = 6 * SHADOWCOUNT_CUBE;
	desc.Format = DSFormat_small_alias;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = USAGE_DEFAULT;
	desc.BindFlags = BIND_DEPTH_STENCIL | BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = RESOURCE_MISC_TEXTURECUBE;
	GetDevice()->CreateTexture2D(&desc, nullptr, &shadowMapArray_Cube);
}
void wiRenderer::DrawForShadowMap(GRAPHICSTHREAD threadID, uint32_t layerMask)
{
	if (wireRender)
		return;

	// We need to render shadows even if the gamespeed is 0 for these reasons:
	// 1.) Shadow cascades is updated every time according to camera
	// 2.) We can move any other light, or object, too

	//if (GetGameSpeed() > 0) 
	{
		GetDevice()->EventBegin("ShadowMap Render", threadID);
		wiProfiler::GetInstance().BeginRange("Shadow Rendering", wiProfiler::DOMAIN_GPU, threadID);

		const bool all_layers = layerMask == 0xFFFFFFFF; // this can avoid the recursive call per object : GetLayerMask()

		const FrameCulling& culling = frameCullings[getCamera()];
		const auto& culledLights = culling.culledLights;

		ViewPort vp;

		// RGB: Shadow tint (multiplicative), A: Refraction caustics(additive)
		const float transparentShadowClearColor[] = { 1,1,1,0 };

		if (!culledLights.empty())
		{
			Scene& scene = GetScene();

			GetDevice()->UnbindResources(TEXSLOT_SHADOWARRAY_2D, 2, threadID);

			int shadowCounter_2D = 0;
			int shadowCounter_Cube = 0;
			for (int type = 0; type < LightComponent::LIGHTTYPE_COUNT; ++type)
			{
				switch (type)
				{
				case LightComponent::DIRECTIONAL:
				case LightComponent::SPOT:
				{
					vp.TopLeftX = 0;
					vp.TopLeftY = 0;
					vp.Width = (float)SHADOWRES_2D;
					vp.Height = (float)SHADOWRES_2D;
					vp.MinDepth = 0.0f;
					vp.MaxDepth = 1.0f;
					GetDevice()->BindViewports(1, &vp, threadID);
					break;
				}
				break;
				case LightComponent::POINT:
				case LightComponent::SPHERE:
				case LightComponent::DISC:
				case LightComponent::RECTANGLE:
				case LightComponent::TUBE:
				{
					vp.TopLeftX = 0;
					vp.TopLeftY = 0;
					vp.Width = (float)SHADOWRES_CUBE;
					vp.Height = (float)SHADOWRES_CUBE;
					vp.MinDepth = 0.0f;
					vp.MaxDepth = 1.0f;
					GetDevice()->BindViewports(1, &vp, threadID);

					GetDevice()->BindConstantBuffer(GS, constantBuffers[CBTYPE_CUBEMAPRENDER], CB_GETBINDSLOT(CubeMapRenderCB), threadID);
					break;
				}
				break;
				default:
					break;
				}

				for (Entity entity : culledLights)
				{
					const LightComponent& light = *scene.lights.GetComponent(entity);
					if (light.GetType() != type || !light.shadow)
					{
						continue;
					}

					switch (type)
					{
					case LightComponent::DIRECTIONAL:
					{
						if ((shadowCounter_2D + 2) >= SHADOWCOUNT_2D || light.shadowMap_index < 0 || light.shadowCam_dirLight.empty())
							break;
						shadowCounter_2D += 3; // shadow indices are already complete so a shadow slot is consumed here even if no rendering actually happens!

						for (int cascade = 0; cascade < 3; ++cascade)
						{
							const float siz = light.shadowCam_dirLight[cascade].size * 0.5f;
							const float f = light.shadowCam_dirLight[cascade].farplane * 0.5f;
							AABB boundingbox;
							boundingbox.createFromHalfWidth(XMFLOAT3(0, 0, 0), XMFLOAT3(siz, siz, f));

							CulledCollection culledRenderer;
							bool transparentShadowsRequested = false;
							for (size_t i = 0; i < scene.aabb_objects.GetCount(); ++i)
							{
								const AABB& aabb = scene.aabb_objects[i];
								if (boundingbox.get(XMMatrixInverse(0, XMLoadFloat4x4(&light.shadowCam_dirLight[cascade].View))).intersects(aabb))
								{
									const ObjectComponent& object = scene.objects[i];
									if (cascade >= object.cascadeMask && object.IsCastingShadow())
									{
										Entity cullable_entity = scene.aabb_objects.GetEntity(i);
										const LayerComponent& layer = *scene.layers.GetComponent(cullable_entity);
										if (all_layers || (layerMask & layer.GetLayerMask()))
										{
											culledRenderer[object.meshID].push_back(cullable_entity);

											if (object.GetRenderTypes() & RENDERTYPE_TRANSPARENT || object.GetRenderTypes() & RENDERTYPE_WATER)
											{
												transparentShadowsRequested = true;
											}
										}
									}
								}
							}
							if (!culledRenderer.empty())
							{
								CameraCB cb;
								cb.mVP = light.shadowCam_dirLight[cascade].getVP();
								GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_CAMERA], &cb, threadID);

								GetDevice()->ClearDepthStencil(shadowMapArray_2D, CLEAR_DEPTH, 0.0f, 0, threadID, light.shadowMap_index + cascade);

								// unfortunately we will always have to clear the associated transparent shadowmap to avoid discrepancy with shadowmap indexing changes across frames
								GetDevice()->ClearRenderTarget(shadowMapArray_Transparent, transparentShadowClearColor, threadID, light.shadowMap_index + cascade);

								// render opaque shadowmap:
								GetDevice()->BindRenderTargets(0, nullptr, shadowMapArray_2D, threadID, light.shadowMap_index + cascade);
								RenderMeshes(light.shadowCam_dirLight[cascade].Eye, culledRenderer, SHADERTYPE_SHADOW, RENDERTYPE_OPAQUE, threadID);

								if (GetTransparentShadowsEnabled() && transparentShadowsRequested)
								{
									// render transparent shadowmap:
									Texture2D* rts[] = {
										shadowMapArray_Transparent
									};
									GetDevice()->BindRenderTargets(ARRAYSIZE(rts), rts, shadowMapArray_2D, threadID, light.shadowMap_index + cascade);
									RenderMeshes(light.shadowCam_dirLight[cascade].Eye, culledRenderer, SHADERTYPE_SHADOW, RENDERTYPE_TRANSPARENT | RENDERTYPE_WATER, threadID);
								}
							}

						}
					}
					break;
					case LightComponent::SPOT:
					{
						if (shadowCounter_2D >= SHADOWCOUNT_2D || light.shadowMap_index < 0 || light.shadowCam_spotLight.empty())
							break;
						shadowCounter_2D++; // shadow indices are already complete so a shadow slot is consumed here even if no rendering actually happens!

						Frustum frustum;
						frustum.ConstructFrustum(light.shadowCam_spotLight[0].farplane, light.shadowCam_spotLight[0].realProjection, light.shadowCam_spotLight[0].View);

						CulledCollection culledRenderer;
						bool transparentShadowsRequested = false;
						for (size_t i = 0; i < scene.aabb_objects.GetCount(); ++i)
						{
							const AABB& aabb = scene.aabb_objects[i];
							if (frustum.CheckBox(aabb))
							{
								const ObjectComponent& object = scene.objects[i];
								if (object.IsCastingShadow())
								{
									Entity cullable_entity = scene.aabb_objects.GetEntity(i);
									const LayerComponent& layer = *scene.layers.GetComponent(cullable_entity);
									if (all_layers || (layerMask & layer.GetLayerMask()))
									{
										culledRenderer[object.meshID].push_back(cullable_entity);

										if (object.GetRenderTypes() & RENDERTYPE_TRANSPARENT || object.GetRenderTypes() & RENDERTYPE_WATER)
										{
											transparentShadowsRequested = true;
										}
									}
								}
							}
						}
						if (!culledRenderer.empty())
						{
							CameraCB cb;
							cb.mVP = light.shadowCam_spotLight[0].getVP();
							GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_CAMERA], &cb, threadID);

							GetDevice()->ClearDepthStencil(shadowMapArray_2D, CLEAR_DEPTH, 0.0f, 0, threadID, light.shadowMap_index);

							// unfortunately we will always have to clear the associated transparent shadowmap to avoid discrepancy with shadowmap indexing changes across frames
							GetDevice()->ClearRenderTarget(shadowMapArray_Transparent, transparentShadowClearColor, threadID, light.shadowMap_index);

							// render opaque shadowmap:
							GetDevice()->BindRenderTargets(0, nullptr, shadowMapArray_2D, threadID, light.shadowMap_index);
							RenderMeshes(frustum.getCamPos(), culledRenderer, SHADERTYPE_SHADOW, RENDERTYPE_OPAQUE, threadID);

							if (GetTransparentShadowsEnabled() && transparentShadowsRequested)
							{
								// render transparent shadowmap:
								Texture2D* rts[] = {
									shadowMapArray_Transparent
								};
								GetDevice()->BindRenderTargets(ARRAYSIZE(rts), rts, shadowMapArray_2D, threadID, light.shadowMap_index);
								RenderMeshes(frustum.getCamPos(), culledRenderer, SHADERTYPE_SHADOW, RENDERTYPE_TRANSPARENT | RENDERTYPE_WATER, threadID);
							}
						}

					}
					break;
					case LightComponent::POINT:
					case LightComponent::SPHERE:
					case LightComponent::DISC:
					case LightComponent::RECTANGLE:
					case LightComponent::TUBE:
					{
						if (shadowCounter_Cube >= SHADOWCOUNT_CUBE || light.shadowMap_index < 0 || light.shadowCam_pointLight.empty())
							break;
						shadowCounter_Cube++; // shadow indices are already complete so a shadow slot is consumed here even if no rendering actually happens!

						CulledCollection culledRenderer;
						for (size_t i = 0; i < scene.aabb_objects.GetCount(); ++i)
						{
							const AABB& aabb = scene.aabb_objects[i];
							if (SPHERE(light.position, light.range).intersects(aabb))
							{
								const ObjectComponent& object = scene.objects[i];
								if (object.IsCastingShadow() && object.GetRenderTypes() == RENDERTYPE_OPAQUE)
								{
									Entity cullable_entity = scene.aabb_objects.GetEntity(i);
									const LayerComponent& layer = *scene.layers.GetComponent(cullable_entity);
									if (all_layers || (layerMask & layer.GetLayerMask()))
									{
										culledRenderer[object.meshID].push_back(cullable_entity);
									}
								}
							}
						}
						if (!culledRenderer.empty())
						{
							GetDevice()->BindRenderTargets(0, nullptr, shadowMapArray_Cube, threadID, light.shadowMap_index);
							GetDevice()->ClearDepthStencil(shadowMapArray_Cube, CLEAR_DEPTH, 0.0f, 0, threadID, light.shadowMap_index);

							MiscCB miscCb;
							miscCb.mColor = XMFLOAT4(light.position.x, light.position.y, light.position.z, 1.0f / light.GetRange()); // reciprocal range, to avoid division in shader
							GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_MISC], &miscCb, threadID);

							CubeMapRenderCB cb;
							for (unsigned int shcam = 0; shcam < light.shadowCam_pointLight.size(); ++shcam)
								cb.mViewProjection[shcam] = light.shadowCam_pointLight[shcam].getVP();

							GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_CUBEMAPRENDER], &cb, threadID);

							RenderMeshes(light.position, culledRenderer, SHADERTYPE_SHADOWCUBE, RENDERTYPE_OPAQUE, threadID);
						}

					}
					break;
					} // terminate switch
				}

			}

			GetDevice()->BindRenderTargets(0, nullptr, nullptr, threadID);
		}


		wiProfiler::GetInstance().EndRange(); // Shadow Rendering
		GetDevice()->EventEnd(threadID);
	}

	GetDevice()->BindResource(PS, shadowMapArray_2D, TEXSLOT_SHADOWARRAY_2D, threadID);
	GetDevice()->BindResource(PS, shadowMapArray_Cube, TEXSLOT_SHADOWARRAY_CUBE, threadID);
	if (GetTransparentShadowsEnabled())
	{
		GetDevice()->BindResource(PS, shadowMapArray_Transparent, TEXSLOT_SHADOWARRAY_TRANSPARENT, threadID);
	}
}

void wiRenderer::RenderMeshes(const XMFLOAT3& eye, const CulledCollection& culledRenderer, SHADERTYPE shaderType, UINT renderTypeFlags, GRAPHICSTHREAD threadID,
	bool tessellation, bool occlusionCulling, uint32_t layerMask)
{
	// Intensive section, refactor and optimize!

	if (!culledRenderer.empty())
	{
		GraphicsDevice* device = GetDevice();
		Scene& scene = GetScene();

		device->EventBegin("RenderMeshes", threadID);

		tessellation = tessellation && device->CheckCapability(GraphicsDevice::GRAPHICSDEVICE_CAPABILITY_TESSELLATION);

		const XMFLOAT4X4 __identityMat = XMFLOAT4X4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);

		struct InstBuf
		{
			Instance instance;
			InstancePrev instancePrev;
		};

		const bool advancedVBRequest =
			!IsWireRender() &&
			(shaderType == SHADERTYPE_FORWARD ||
			shaderType == SHADERTYPE_DEFERRED ||
			shaderType == SHADERTYPE_TILEDFORWARD);

		const bool easyTextureBind = 
			shaderType == SHADERTYPE_TEXTURE || 
			shaderType == SHADERTYPE_SHADOW || 
			shaderType == SHADERTYPE_SHADOWCUBE || 
			shaderType == SHADERTYPE_DEPTHONLY || 
			shaderType == SHADERTYPE_VOXELIZE;

		const bool all_layers = layerMask == 0xFFFFFFFF; // this can avoid the recursive call per object : GetLayerMask()

		GraphicsPSO* impostorRequest = GetImpostorPSO(shaderType);

		// Render impostors:
		if (impostorRequest != nullptr)
		{
			bool impostorGraphicsStateComplete = false;

			for (CulledCollection::const_iterator iter = culledRenderer.begin(); iter != culledRenderer.end(); ++iter)
			{
				Entity meshEntity = iter->first;
				MeshComponent& mesh = *scene.meshes.GetComponent(meshEntity);
				if (!mesh.renderable || !mesh.HasImpostor() /*|| !(mesh.GetRenderTypes() & renderTypeFlags)*/)
				{
					continue;
				}

				const auto& visibleInstances = iter->second;

				UINT instancesOffset;
				size_t alloc_size = visibleInstances.size();
				alloc_size *= advancedVBRequest ? sizeof(InstBuf) : sizeof(Instance);
				void* instances = device->AllocateFromRingBuffer(dynamicVertexBufferPool, alloc_size, instancesOffset, threadID);

				int k = 0;
				for (const Entity objectEntity : visibleInstances)
				{
					const ObjectComponent& instance = *scene.objects.GetComponent(objectEntity);
					if (occlusionCulling && instance.IsOccluded())
						continue;

					const LayerComponent& layer = *scene.layers.GetComponent(objectEntity);

					if (all_layers || (layerMask & layer.GetLayerMask()))
					{
						const AABB& aabb = *scene.aabb_objects.GetComponent(objectEntity);

						const float impostorThreshold = aabb.getRadius();
						float dist = wiMath::Distance(eye, aabb.getCenter());
						float dither = instance.GetTransparency();
						dither = wiMath::SmoothStep(1.0f, dither, wiMath::Clamp((dist - mesh.impostorDistance) / impostorThreshold, 0, 1));
						if (dither > 1.0f - FLT_EPSILON)
							continue;

						XMMATRIX boxMat = mesh.aabb.getAsBoxMatrix();

						const TransformComponent& transform = *scene.transforms.GetComponent(objectEntity);

						XMFLOAT4X4 tempMat;
						XMStoreFloat4x4(&tempMat, boxMat*XMLoadFloat4x4(&transform.world));

						if (advancedVBRequest)
						{
							((volatile InstBuf*)instances)[k].instance.Create(tempMat, instance.color, dither);

							const PreviousFrameTransformComponent& prev_transform = *scene.prev_transforms.GetComponent(objectEntity);
							XMStoreFloat4x4(&tempMat, boxMat*XMLoadFloat4x4(&prev_transform.world_prev));
							((volatile InstBuf*)instances)[k].instancePrev.Create(tempMat);
						}
						else
						{
							((volatile Instance*)instances)[k].Create(tempMat, instance.color, dither);
						}

						++k;
					}
				}

				device->InvalidateBufferAccess(dynamicVertexBufferPool, threadID);

				if (k < 1)
					continue;

				if (!advancedVBRequest || IsWireRender())
				{
					GPUBuffer* vbs[] = {
						&impostorVB_POS,
						&impostorVB_TEX,
						dynamicVertexBufferPool
					};
					UINT strides[] = {
						sizeof(MeshComponent::Vertex_POS),
						sizeof(MeshComponent::Vertex_TEX),
						sizeof(Instance)
					};
					UINT offsets[] = {
						0,
						0,
						instancesOffset
					};
					device->BindVertexBuffers(vbs, 0, ARRAYSIZE(vbs), strides, offsets, threadID);
				}
				else
				{
					GPUBuffer* vbs[] = {
						&impostorVB_POS,
						&impostorVB_TEX,
						&impostorVB_POS,
						dynamicVertexBufferPool
					};
					UINT strides[] = {
						sizeof(MeshComponent::Vertex_POS),
						sizeof(MeshComponent::Vertex_TEX),
						sizeof(MeshComponent::Vertex_POS),
						sizeof(InstBuf)
					};
					UINT offsets[] = {
						0,
						0,
						0,
						instancesOffset
					};
					device->BindVertexBuffers(vbs, 0, ARRAYSIZE(vbs), strides, offsets, threadID);
				}

				GPUResource* res[] = {
					mesh.impostorTarget.GetTexture(0),
					mesh.impostorTarget.GetTexture(1),
					mesh.impostorTarget.GetTexture(2)
				};
				device->BindResources(PS, res, TEXSLOT_ONDEMAND0, (easyTextureBind ? 1 : ARRAYSIZE(res)), threadID);

				if (!impostorGraphicsStateComplete)
				{
					device->BindGraphicsPSO(impostorRequest, threadID);
					device->BindConstantBuffer(PS, &impostorMaterialCB, CB_GETBINDSLOT(MaterialCB), threadID);
					SetAlphaRef(0.75f, threadID);
					impostorGraphicsStateComplete = true;
				}

				device->DrawInstanced(6 * 6, k, 0, 0, threadID); // 6 * 6: see MeshComponent::CreateImpostorVB function

			}
		}


		PRIMITIVETOPOLOGY prevTOPOLOGY = TRIANGLELIST;

		// Render meshes:
		for (CulledCollection::const_iterator iter = culledRenderer.begin(); iter != culledRenderer.end(); ++iter)
		{
			Entity meshEntity = iter->first;
			MeshComponent& mesh = *scene.meshes.GetComponent(meshEntity);
			if (!mesh.renderable /*|| !(mesh.GetRenderTypes() & renderTypeFlags)*/)
			{
				continue;
			}

			const auto& visibleInstances = iter->second;

			const float tessF = mesh.GetTessellationFactor();
			const bool tessellatorRequested = tessF > 0 && tessellation;

			if (tessellatorRequested)
			{
				TessellationCB tessCB;
				tessCB.tessellationFactors = XMFLOAT4(tessF, tessF, tessF, tessF);
				device->UpdateBuffer(constantBuffers[CBTYPE_TESSELLATION], &tessCB, threadID);
				device->BindConstantBuffer(HS, constantBuffers[CBTYPE_TESSELLATION], CBSLOT_RENDERER_TESSELLATION, threadID);
			}

			bool forceAlphaTestForDithering = false;

			UINT instancesOffset;
			size_t alloc_size = visibleInstances.size();
			alloc_size *= advancedVBRequest ? sizeof(InstBuf) : sizeof(Instance);
			void* instances = device->AllocateFromRingBuffer(dynamicVertexBufferPool, alloc_size, instancesOffset, threadID);

			int k = 0;
			for (const Entity objectEntity : visibleInstances)
			{
				const ObjectComponent& instance = *scene.objects.GetComponent(objectEntity);
				if (occlusionCulling && instance.IsOccluded())
					continue;

				const LayerComponent& layer = *scene.layers.GetComponent(objectEntity);

				if (all_layers || (layerMask & layer.GetLayerMask()))
				{
					const AABB& aabb = *scene.aabb_objects.GetComponent(objectEntity);

					float dither = instance.GetTransparency();
					if (impostorRequest != nullptr)
					{
						// fade out to impostor...
						const float impostorThreshold = aabb.getRadius();
						float dist = wiMath::Distance(eye, aabb.getCenter());
						if (mesh.HasImpostor())
							dither = wiMath::SmoothStep(dither, 1.0f, wiMath::Clamp((dist - impostorThreshold - mesh.impostorDistance) / impostorThreshold, 0, 1));
					}
					if (dither > 1.0f - FLT_EPSILON)
						continue;

					forceAlphaTestForDithering = forceAlphaTestForDithering || (dither > 0);

					const TransformComponent& transform = *scene.transforms.GetComponent(objectEntity);

					if (advancedVBRequest || tessellatorRequested)
					{
						((volatile InstBuf*)instances)[k].instance.Create(transform.world, instance.color);

						const PreviousFrameTransformComponent& prev_transform = *scene.prev_transforms.GetComponent(objectEntity);
						((volatile InstBuf*)instances)[k].instancePrev.Create(prev_transform.world_prev);
					}
					else
					{
						((volatile Instance*)instances)[k].Create(transform.world, instance.color, dither);
					}

					++k;
				}
			}

			device->InvalidateBufferAccess(dynamicVertexBufferPool, threadID);

			if (k < 1)
				continue;

			device->BindIndexBuffer(mesh.indexBuffer.get(), mesh.GetIndexFormat(), 0, threadID);

			enum class BOUNDVERTEXBUFFERTYPE
			{
				NOTHING,
				POSITION,
				POSITION_TEXCOORD,
				EVERYTHING,
			};
			BOUNDVERTEXBUFFERTYPE boundVBType_Prev = BOUNDVERTEXBUFFERTYPE::NOTHING;

			for (MeshComponent::MeshSubset& subset : mesh.subsets)
			{
				if (subset.indexCount == 0)
				{
					continue;
				}
				const MaterialComponent& material = *scene.materials.GetComponent(subset.materialID);

				GraphicsPSO* pso = /*material.customShader == nullptr ?*/ GetObjectPSO(shaderType, mesh.doubleSided, tessellatorRequested, material, forceAlphaTestForDithering) /*: material.customShader->passes[shaderType].pso*/;
				if (pso == nullptr)
				{
					continue;
				}

				bool subsetRenderable = false;

				if (renderTypeFlags & RENDERTYPE_OPAQUE)
				{
					subsetRenderable = subsetRenderable || (!material.IsTransparent() && !material.IsWater());
				}
				if (renderTypeFlags & RENDERTYPE_TRANSPARENT)
				{
					subsetRenderable = subsetRenderable || material.IsTransparent();
				}
				if (renderTypeFlags & RENDERTYPE_WATER)
				{
					subsetRenderable = subsetRenderable || material.IsWater();
				}
				if (shaderType == SHADERTYPE_SHADOW || shaderType == SHADERTYPE_SHADOWCUBE)
				{
					subsetRenderable = subsetRenderable && material.IsCastingShadow();
				}

				if (!subsetRenderable)
				{
					continue;
				}

				BOUNDVERTEXBUFFERTYPE boundVBType;
				if (advancedVBRequest || tessellatorRequested)
				{
					boundVBType = BOUNDVERTEXBUFFERTYPE::EVERYTHING;
				}
				else
				{
					// simple vertex buffers are used in some passes (note: tessellator requires more attributes)
					if ((shaderType == SHADERTYPE_DEPTHONLY || shaderType == SHADERTYPE_SHADOW || shaderType == SHADERTYPE_SHADOWCUBE) && !material.IsAlphaTestEnabled() && !forceAlphaTestForDithering)
					{
						if (shaderType == SHADERTYPE_SHADOW && material.IsTransparent())
						{
							boundVBType = BOUNDVERTEXBUFFERTYPE::POSITION_TEXCOORD;
						}
						else
						{
							// bypass texcoord stream for non alphatested shadows and zprepass
							boundVBType = BOUNDVERTEXBUFFERTYPE::POSITION;
						}
					}
					else
					{
						boundVBType = BOUNDVERTEXBUFFERTYPE::POSITION_TEXCOORD;
					}
				}

				if (material.IsWater())
				{
					boundVBType = BOUNDVERTEXBUFFERTYPE::POSITION_TEXCOORD;
				}

				if (IsWireRender())
				{
					boundVBType = BOUNDVERTEXBUFFERTYPE::POSITION_TEXCOORD;
				}

				// Only bind vertex buffers when the layout changes
				if (boundVBType != boundVBType_Prev)
				{
					// Assemble the required vertex buffer:
					switch (boundVBType)
					{
					case BOUNDVERTEXBUFFERTYPE::POSITION:
					{
						GPUBuffer* vbs[] = {
							mesh.streamoutBuffer_POS.get() != nullptr ? mesh.streamoutBuffer_POS.get() : mesh.vertexBuffer_POS.get(),
							dynamicVertexBufferPool
						};
						UINT strides[] = {
							sizeof(MeshComponent::Vertex_POS),
							sizeof(Instance)
						};
						UINT offsets[] = {
							0,
							instancesOffset
						};
						device->BindVertexBuffers(vbs, 0, ARRAYSIZE(vbs), strides, offsets, threadID);
					}
					break;
					case BOUNDVERTEXBUFFERTYPE::POSITION_TEXCOORD:
					{
						GPUBuffer* vbs[] = {
							mesh.streamoutBuffer_POS.get() != nullptr ? mesh.streamoutBuffer_POS.get() : mesh.vertexBuffer_POS.get(),
							mesh.vertexBuffer_TEX.get(),
							dynamicVertexBufferPool
						};
						UINT strides[] = {
							sizeof(MeshComponent::Vertex_POS),
							sizeof(MeshComponent::Vertex_TEX),
							sizeof(Instance)
						};
						UINT offsets[] = {
							0,
							0,
							instancesOffset
						};
						device->BindVertexBuffers(vbs, 0, ARRAYSIZE(vbs), strides, offsets, threadID);
					}
					break;
					case BOUNDVERTEXBUFFERTYPE::EVERYTHING:
					{
						GPUBuffer* vbs[] = {
							mesh.streamoutBuffer_POS.get() != nullptr ? mesh.streamoutBuffer_POS.get() : mesh.vertexBuffer_POS.get(),
							mesh.vertexBuffer_TEX.get(),
							mesh.streamoutBuffer_PRE.get() != nullptr ? mesh.streamoutBuffer_PRE.get() : mesh.vertexBuffer_POS.get(),
							dynamicVertexBufferPool
						};
						UINT strides[] = {
							sizeof(MeshComponent::Vertex_POS),
							sizeof(MeshComponent::Vertex_TEX),
							sizeof(MeshComponent::Vertex_POS),
							sizeof(InstBuf)
						};
						UINT offsets[] = {
							0,
							0,
							0,
							instancesOffset
						};
						device->BindVertexBuffers(vbs, 0, ARRAYSIZE(vbs), strides, offsets, threadID);
					}
					break;
					default:
						assert(0);
						break;
					}
				}
				boundVBType_Prev = boundVBType;

				device->BindConstantBuffer(PS, material.constantBuffer.get(), CB_GETBINDSLOT(MaterialCB), threadID);

				device->BindStencilRef(material.GetStencilRef(), threadID);

				device->BindGraphicsPSO(pso, threadID);

				GPUResource* res[] = {
					material.GetBaseColorMap(),
					material.GetNormalMap(),
					material.GetSurfaceMap(),
					material.GetDisplacementMap(),
				};
				device->BindResources(PS, res, TEXSLOT_ONDEMAND0, (easyTextureBind ? 2 : ARRAYSIZE(res)), threadID);

				SetAlphaRef(material.alphaRef, threadID);

				device->DrawIndexedInstanced((int)subset.indexCount, k, subset.indexOffset, 0, 0, threadID);
			}

		}

		ResetAlphaRef(threadID);

		device->EventEnd(threadID);
	}
}

void wiRenderer::DrawWorld(CameraComponent* camera, bool tessellation, GRAPHICSTHREAD threadID, SHADERTYPE shaderType, bool grass, bool occlusionCulling, uint32_t layerMask)
{
	const FrameCulling& culling = frameCullings[camera];

	GetDevice()->EventBegin("DrawWorld", threadID);

	if (shaderType == SHADERTYPE_TILEDFORWARD)
	{
		GetDevice()->BindResource(PS, resourceBuffers[RBTYPE_ENTITYINDEXLIST_OPAQUE], SBSLOT_ENTITYINDEXLIST, threadID);
	}

	if (grass)
	{
		if (GetAlphaCompositionEnabled())
		{
			// cut off most transparent areas
			SetAlphaRef(0.25f, threadID);
		}

		Scene& scene = GetScene();

		for (size_t i = 0; i < scene.hairs.GetCount(); ++i)
		{
			const wiHairParticle& hair = scene.hairs[i];

			if (camera->frustum.CheckBox(hair.aabb))
			{
				Entity entity = scene.hairs.GetEntity(i);
				const MaterialComponent& material = *scene.materials.GetComponent(entity);

				hair.Draw(camera, material, shaderType, false, threadID);
			}
		}
	}

	if (!culling.culledRenderer_opaque.empty()/* || (grass && culling.culledHairParticleSystems.empty())*/)
	{
		RenderMeshes(camera->Eye, culling.culledRenderer_opaque, shaderType, RENDERTYPE_OPAQUE, threadID, tessellation, GetOcclusionCullingEnabled() && occlusionCulling, layerMask);
	}

	GetDevice()->EventEnd(threadID);

}

void wiRenderer::DrawWorldTransparent(CameraComponent* camera, SHADERTYPE shaderType, GRAPHICSTHREAD threadID, bool grass, bool occlusionCulling, uint32_t layerMask)
{
	const FrameCulling& culling = frameCullings[camera];

	GetDevice()->EventBegin("DrawWorldTransparent", threadID);

	if (shaderType == SHADERTYPE_TILEDFORWARD)
	{
		GetDevice()->BindResource(PS, resourceBuffers[RBTYPE_ENTITYINDEXLIST_TRANSPARENT], SBSLOT_ENTITYINDEXLIST, threadID);
	}

	if (ocean != nullptr)
	{
		ocean->Render(camera, renderTime, threadID);
	}

	if (grass && GetAlphaCompositionEnabled())
	{
		// transparent passes can only render hair when alpha composition is enabled
		Scene& scene = GetScene();

		for (size_t i = 0; i < scene.hairs.GetCount(); ++i)
		{
			const wiHairParticle& hair = scene.hairs[i];

			if (camera->frustum.CheckBox(hair.aabb))
			{
				Entity entity = scene.hairs.GetEntity(i);
				const MaterialComponent& material = *scene.materials.GetComponent(entity);

				hair.Draw(camera, material, shaderType, true, threadID);
			}
		}
	}

	if (!culling.culledRenderer_transparent.empty())
	{
		RenderMeshes(camera->Eye, culling.culledRenderer_transparent, shaderType, RENDERTYPE_TRANSPARENT | RENDERTYPE_WATER, threadID, false, GetOcclusionCullingEnabled() && occlusionCulling, layerMask);
	}

	GetDevice()->EventEnd(threadID);
}


void wiRenderer::DrawSky(GRAPHICSTHREAD threadID)
{
	GetDevice()->EventBegin("DrawSky", threadID);
	
	if (enviroMap != nullptr)
	{
		GetDevice()->BindGraphicsPSO(PSO_sky[SKYRENDERING_STATIC], threadID);
		GetDevice()->BindResource(PS, enviroMap, TEXSLOT_ONDEMAND0, threadID);
	}
	else
	{
		GetDevice()->BindGraphicsPSO(PSO_sky[SKYRENDERING_DYNAMIC], threadID);
		if (GetScene().cloudiness > 0)
		{
			GetDevice()->BindResource(PS, textures[TEXTYPE_2D_CLOUDS], TEXSLOT_ONDEMAND0, threadID);
		}
		else
		{
			GetDevice()->BindResource(PS, wiTextureHelper::getInstance()->getBlack(), TEXSLOT_ONDEMAND0, threadID);
		}
	}

	GetDevice()->Draw(240, 0, threadID);

	GetDevice()->EventEnd(threadID);
}
void wiRenderer::DrawSun(GRAPHICSTHREAD threadID)
{
	GetDevice()->EventBegin("DrawSun", threadID);

	GetDevice()->BindGraphicsPSO(PSO_sky[SKYRENDERING_SUN], threadID);

	if (enviroMap != nullptr)
	{
		GetDevice()->BindResource(PS, wiTextureHelper::getInstance()->getBlack(), TEXSLOT_ONDEMAND0, threadID);
	}
	else
	{
		GetDevice()->BindResource(PS, textures[TEXTYPE_2D_CLOUDS], TEXSLOT_ONDEMAND0, threadID);
	}

	GetDevice()->Draw(240, 0, threadID);

	GetDevice()->EventEnd(threadID);
}

void wiRenderer::DrawDecals(CameraComponent* camera, GRAPHICSTHREAD threadID)
{
	GraphicsDevice* device = GetDevice();
	Scene& scene = GetScene();

	bool boundCB = false;
	if(scene.decals.GetCount() > 0)
	{
		device->EventBegin("Decals", threadID);

		if (!boundCB)
		{
			boundCB = true;
			device->BindConstantBuffer(PS, constantBuffers[CBTYPE_DECAL], CB_GETBINDSLOT(DecalCB),threadID);
		}

		device->BindStencilRef(STENCILREF_DEFAULT, threadID);

		device->BindGraphicsPSO(PSO_decal, threadID);

		for (size_t i = 0; i < scene.decals.GetCount(); ++i) 
		{
			DecalComponent& decal = scene.decals[i];
			Entity entity = scene.decals.GetEntity(i);
			const AABB& aabb = *scene.aabb_decals.GetComponent(entity);

			if ((decal.texture != nullptr || decal.normal != nullptr) && camera->frustum.CheckBox(aabb)) 
			{

				device->BindResource(PS, decal.texture, TEXSLOT_ONDEMAND0, threadID);
				device->BindResource(PS, decal.normal, TEXSLOT_ONDEMAND1, threadID);

				XMMATRIX decalWorld = XMLoadFloat4x4(&decal.world);

				MiscCB dcbvs;
				dcbvs.mTransform = XMMatrixTranspose(decalWorld*camera->GetViewProjection());
				device->UpdateBuffer(constantBuffers[CBTYPE_MISC], &dcbvs, threadID);

				DecalCB dcbps;
				dcbps.mDecalVP = XMMatrixTranspose(XMMatrixInverse(nullptr, decalWorld)); // todo: cache the inverse!
				dcbps.hasTexNor = 0;
				if (decal.texture != nullptr)
					dcbps.hasTexNor |= 0x0000001;
				if (decal.normal != nullptr)
					dcbps.hasTexNor |= 0x0000010;
				XMStoreFloat3(&dcbps.eye, camera->GetEye());
				dcbps.opacity = decal.GetOpacity();
				dcbps.front = decal.front;
				device->UpdateBuffer(constantBuffers[CBTYPE_DECAL], &dcbps, threadID);

				device->Draw(14, 0, threadID);

			}

		}

		device->EventEnd(threadID);
	}
}

void wiRenderer::RefreshEnvProbes(GRAPHICSTHREAD threadID)
{
	GetDevice()->EventBegin("EnvironmentProbe Refresh", threadID);

	Scene& scene = GetScene();

	static const UINT envmapRes = 128;
	static const UINT envmapCount = 16;
	static const UINT envmapMIPs = 8;

	if (textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY] == nullptr)
	{
		TextureDesc desc;
		desc.ArraySize = envmapCount * 6;
		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET | BIND_UNORDERED_ACCESS;
		desc.CPUAccessFlags = 0;
		desc.Format = RTFormat_hdr;
		desc.Height = envmapRes;
		desc.Width = envmapRes;
		desc.MipLevels = envmapMIPs;
		desc.MiscFlags = RESOURCE_MISC_TEXTURECUBE /*| RESOURCE_MISC_GENERATE_MIPS*/;
		desc.Usage = USAGE_DEFAULT;

		textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY] = new Texture2D;
		textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY]->RequestIndependentRenderTargetArraySlices(true);
		textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY]->RequestIndependentShaderResourceArraySlices(true);
		textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY]->RequestIndependentShaderResourcesForMIPs(true);
		textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY]->RequestIndependentUnorderedAccessResourcesForMIPs(true);
		HRESULT hr = GetDevice()->CreateTexture2D(&desc, nullptr, (Texture2D**)&textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY]);
		assert(SUCCEEDED(hr));
	}

	static Texture2D* envrenderingDepthBuffer = nullptr;
	if (envrenderingDepthBuffer == nullptr)
	{
		TextureDesc desc;
		desc.ArraySize = 6;
		desc.BindFlags = BIND_DEPTH_STENCIL;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_D16_UNORM;
		desc.Height = envmapRes;
		desc.Width = envmapRes;
		desc.MipLevels = 1;
		desc.MiscFlags = RESOURCE_MISC_TEXTURECUBE;
		desc.Usage = USAGE_DEFAULT;

		HRESULT hr = GetDevice()->CreateTexture2D(&desc, nullptr, &envrenderingDepthBuffer);
		assert(SUCCEEDED(hr));
	}

	ViewPort VP;
	VP.Height = envmapRes;
	VP.Width = envmapRes;
	VP.TopLeftX = 0;
	VP.TopLeftY = 0;
	VP.MinDepth = 0.0f;
	VP.MaxDepth = 1.0f;
	GetDevice()->BindViewports(1, &VP, threadID);

	const float zNearP = getCamera()->zNearP;
	const float zFarP = getCamera()->zFarP;

	// reconstruct envmap array status:
	bool envmapTaken[envmapCount] = {};
	for (size_t i = 0; i < scene.probes.GetCount(); ++i)
	{
		const EnvironmentProbeComponent& probe = scene.probes[i];
		if (probe.textureIndex >= 0)
		{
			envmapTaken[probe.textureIndex] = true;
		}
	}

	for (size_t i = 0; i < scene.probes.GetCount(); ++i)
	{
		EnvironmentProbeComponent& probe = scene.probes[i];
		Entity entity = scene.probes.GetEntity(i);

		if (probe.textureIndex < 0)
		{
			// need to take a free envmap texture slot:
			bool found = false;
			for (int i = 0; i < ARRAYSIZE(envmapTaken); ++i)
			{
				if (envmapTaken[i] == false)
				{
					envmapTaken[i] = true;
					probe.textureIndex = i;
					found = true;
					break;
				}
			}
			if (!found)
			{
				// could not find free slot in envmap array, so skip this probe:
				continue;
			}
		}

		if (probe.isUpToDate)
		{
			continue;
		}
		if (!probe.realTime)
		{
			probe.isUpToDate = true;
		}

		GetDevice()->BindRenderTargets(1, (Texture2D**)&textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY], envrenderingDepthBuffer, threadID, probe.textureIndex);
		const float clearColor[4] = { 0,0,0,1 };
		GetDevice()->ClearRenderTarget(textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY], clearColor, threadID, probe.textureIndex);
		GetDevice()->ClearDepthStencil(envrenderingDepthBuffer, CLEAR_DEPTH, 0.0f, 0, threadID);


		std::vector<LightComponent::SHCAM> cameras;
		{
			cameras.clear();

			cameras.push_back(LightComponent::SHCAM(XMFLOAT4(0.5f, -0.5f, -0.5f, -0.5f), zNearP, zFarP, XM_PI / 2.0f)); //+x
			cameras.push_back(LightComponent::SHCAM(XMFLOAT4(0.5f, 0.5f, 0.5f, -0.5f), zNearP, zFarP, XM_PI / 2.0f)); //-x

			cameras.push_back(LightComponent::SHCAM(XMFLOAT4(1, 0, 0, -0), zNearP, zFarP, XM_PI / 2.0f)); //+y
			cameras.push_back(LightComponent::SHCAM(XMFLOAT4(0, 0, 0, -1), zNearP, zFarP, XM_PI / 2.0f)); //-y

			cameras.push_back(LightComponent::SHCAM(XMFLOAT4(0.707f, 0, 0, -0.707f), zNearP, zFarP, XM_PI / 2.0f)); //+z
			cameras.push_back(LightComponent::SHCAM(XMFLOAT4(0, 0.707f, 0.707f, 0), zNearP, zFarP, XM_PI / 2.0f)); //-z
		}

		XMFLOAT3 center = probe.position;
		XMVECTOR vCenter = XMLoadFloat3(&center);

		CubeMapRenderCB cb;
		for (unsigned int i = 0; i < cameras.size(); ++i)
		{
			cameras[i].Update(vCenter);
			cb.mViewProjection[i] = cameras[i].getVP();
		}

		GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_CUBEMAPRENDER], &cb, threadID);
		GetDevice()->BindConstantBuffer(GS, constantBuffers[CBTYPE_CUBEMAPRENDER], CB_GETBINDSLOT(CubeMapRenderCB), threadID);


		CameraCB camcb;
		camcb.mCamPos = center; // only this will be used by envprobe rendering shaders the rest is read from cubemaprenderCB
		GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_CAMERA], &camcb, threadID);

		const LayerComponent& layer = *scene.layers.GetComponent(entity);
		const uint32_t layerMask = layer.GetLayerMask();

		SPHERE culler = SPHERE(center, zFarP);

		CulledCollection culledRenderer;
		for (size_t i = 0; i < scene.aabb_objects.GetCount(); ++i)
		{
			const AABB& aabb = scene.aabb_objects[i];
			if (culler.intersects(aabb))
			{
				Entity cullable_entity = scene.aabb_objects.GetEntity(i);
				const LayerComponent& layer = *scene.layers.GetComponent(cullable_entity);
				if ((layerMask & layer.GetLayerMask()))
				{
					const ObjectComponent& object = scene.objects[i];
					culledRenderer[object.meshID].push_back(cullable_entity);
				}
			}
		}

		if (!culledRenderer.empty())
		{
			RenderMeshes(center, culledRenderer, SHADERTYPE_ENVMAPCAPTURE, RENDERTYPE_OPAQUE | RENDERTYPE_TRANSPARENT, threadID);
		}

		// sky
		{

			if (enviroMap != nullptr)
			{
				GetDevice()->BindGraphicsPSO(PSO_sky[SKYRENDERING_ENVMAPCAPTURE_STATIC], threadID);
				GetDevice()->BindResource(PS, enviroMap, TEXSLOT_ONDEMAND0, threadID);
			}
			else
			{
				GetDevice()->BindGraphicsPSO(PSO_sky[SKYRENDERING_ENVMAPCAPTURE_DYNAMIC], threadID);
				GetDevice()->BindResource(PS, textures[TEXTYPE_2D_CLOUDS], TEXSLOT_ONDEMAND0, threadID);
			}

			GetDevice()->Draw(240, 0, threadID);
		}

		GetDevice()->BindRenderTargets(0, nullptr, nullptr, threadID);
		//GetDevice()->GenerateMips(textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY], threadID, probe->textureIndex);
		wiRenderer::GenerateMipChain((Texture2D*)textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY], MIPGENFILTER_LINEAR, threadID, probe.textureIndex);

		// Filter the enviroment map mip chain according to BRDF:
		//	A bit similar to MIP chain generation, but its input is the MIP-mapped texture,
		//	and we generatethe filtered MIPs from bottom to top.
		GetDevice()->EventBegin("FilterEnvMap", threadID);
		{
			Texture* texture = textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY];
			TextureDesc desc = texture->GetDesc();
			int arrayIndex = probe.textureIndex;

			GetDevice()->BindComputePSO(CPSO[CSTYPE_FILTERENVMAP], threadID);

			desc.Width = 1;
			desc.Height = 1;
			for (UINT i = desc.MipLevels - 1; i > 0; --i)
			{
				GetDevice()->BindUAV(CS, texture, 0, threadID, i);
				GetDevice()->BindResource(CS, texture, TEXSLOT_UNIQUE0, threadID, max(0, (int)i - 2));

				FilterEnvmapCB cb;
				cb.filterResolution.x = desc.Width;
				cb.filterResolution.y = desc.Height;
				cb.filterArrayIndex = arrayIndex;
				cb.filterRoughness = (float)i / (float)desc.MipLevels;
				cb.filterRayCount = 128;
				GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_FILTERENVMAP], &cb, threadID);
				GetDevice()->BindConstantBuffer(CS, constantBuffers[CBTYPE_FILTERENVMAP], CB_GETBINDSLOT(FilterEnvmapCB), threadID);

				GetDevice()->Dispatch(
					max(1, (UINT)ceilf((float)desc.Width / GENERATEMIPCHAIN_2D_BLOCK_SIZE)),
					max(1, (UINT)ceilf((float)desc.Height / GENERATEMIPCHAIN_2D_BLOCK_SIZE)),
					6,
					threadID);

				GetDevice()->UAVBarrier((GPUResource**)&texture, 1, threadID);

				desc.Width *= 2;
				desc.Height *= 2;
			}
			GetDevice()->UnbindUAVs(0, 1, threadID);
		}
		GetDevice()->EventEnd(threadID);

	}

	GetDevice()->BindResource(PS, textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY], TEXSLOT_ENVMAPARRAY, threadID);
	GetDevice()->BindResource(CS, textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY], TEXSLOT_ENVMAPARRAY, threadID);

	GetDevice()->EventEnd(threadID); // EnvironmentProbe Refresh
}

void wiRenderer::VoxelRadiance(GRAPHICSTHREAD threadID)
{
	if (!GetVoxelRadianceEnabled())
	{
		return;
	}

	GraphicsDevice* device = GetDevice();

	device->EventBegin("Voxel Radiance", threadID);
	wiProfiler::GetInstance().BeginRange("Voxel Radiance", wiProfiler::DOMAIN_GPU, threadID);

	Scene& scene = GetScene();

	if (textures[TEXTYPE_3D_VOXELRADIANCE] == nullptr)
	{
		TextureDesc desc;
		ZeroMemory(&desc, sizeof(desc));
		desc.Width = voxelSceneData.res;
		desc.Height = voxelSceneData.res;
		desc.Depth = voxelSceneData.res;
		desc.MipLevels = 0;
		desc.Format = RTFormat_voxelradiance;
		desc.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
		desc.Usage = USAGE_DEFAULT;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;

		textures[TEXTYPE_3D_VOXELRADIANCE] = new Texture3D;
		textures[TEXTYPE_3D_VOXELRADIANCE]->RequestIndependentShaderResourcesForMIPs(true);
		textures[TEXTYPE_3D_VOXELRADIANCE]->RequestIndependentUnorderedAccessResourcesForMIPs(true);
		HRESULT hr = device->CreateTexture3D(&desc, nullptr, (Texture3D**)&textures[TEXTYPE_3D_VOXELRADIANCE]);
		assert(SUCCEEDED(hr));
	}
	if (voxelSceneData.secondaryBounceEnabled && textures[TEXTYPE_3D_VOXELRADIANCE_HELPER] == nullptr)
	{
		TextureDesc desc = ((Texture3D*)textures[TEXTYPE_3D_VOXELRADIANCE])->GetDesc();
		textures[TEXTYPE_3D_VOXELRADIANCE_HELPER] = new Texture3D;
		textures[TEXTYPE_3D_VOXELRADIANCE_HELPER]->RequestIndependentShaderResourcesForMIPs(true);
		textures[TEXTYPE_3D_VOXELRADIANCE_HELPER]->RequestIndependentUnorderedAccessResourcesForMIPs(true);
		HRESULT hr = device->CreateTexture3D(&desc, nullptr, (Texture3D**)&textures[TEXTYPE_3D_VOXELRADIANCE_HELPER]);
		assert(SUCCEEDED(hr));
	}
	if (resourceBuffers[RBTYPE_VOXELSCENE] == nullptr)
	{
		GPUBufferDesc desc;
		desc.StructureByteStride = sizeof(UINT) * 2;
		desc.ByteWidth = desc.StructureByteStride * voxelSceneData.res * voxelSceneData.res * voxelSceneData.res;
		desc.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;

		resourceBuffers[RBTYPE_VOXELSCENE] = new GPUBuffer;
		HRESULT hr = device->CreateBuffer(&desc, nullptr, resourceBuffers[RBTYPE_VOXELSCENE]);
		assert(SUCCEEDED(hr));
	}

	Texture3D* result = (Texture3D*)textures[TEXTYPE_3D_VOXELRADIANCE];

	AABB bbox;
	XMFLOAT3 extents = voxelSceneData.extents;
	XMFLOAT3 center = voxelSceneData.center;
	bbox.createFromHalfWidth(center, extents);


	CulledCollection culledRenderer;
	for (size_t i = 0; i < scene.aabb_objects.GetCount(); ++i)
	{
		const AABB& aabb = scene.aabb_objects[i];
		if (bbox.intersects(aabb))
		{
			Entity cullable_entity = scene.aabb_objects.GetEntity(i);
			const ObjectComponent& object = scene.objects[i];
			culledRenderer[object.meshID].push_back(cullable_entity);
		}
	}

	if (!culledRenderer.empty())
	{
		ViewPort VP;
		VP.TopLeftX = 0;
		VP.TopLeftY = 0;
		VP.Width = (float)voxelSceneData.res;
		VP.Height = (float)voxelSceneData.res;
		VP.MinDepth = 0.0f;
		VP.MaxDepth = 1.0f;
		device->BindViewports(1, &VP, threadID);

		GPUResource* UAVs[] = { resourceBuffers[RBTYPE_VOXELSCENE] };
		device->BindUAVs(PS, UAVs, 0, 1, threadID);

		RenderMeshes(center, culledRenderer, SHADERTYPE_VOXELIZE, RENDERTYPE_OPAQUE, threadID);

		// Copy the packed voxel scene data to a 3D texture, then delete the voxel scene emission data. The cone tracing will operate on the 3D texture
		device->EventBegin("Voxel Scene Copy - Clear", threadID);
		device->BindRenderTargets(0, nullptr, nullptr, threadID);
		device->BindUAV(CS, resourceBuffers[RBTYPE_VOXELSCENE], 0, threadID);
		device->BindUAV(CS, textures[TEXTYPE_3D_VOXELRADIANCE], 1, threadID);

		if (device->CheckCapability(GraphicsDevice::GRAPHICSDEVICE_CAPABILITY_UNORDEREDACCESSTEXTURE_LOAD_FORMAT_EXT))
		{
			device->BindComputePSO(CPSO[CSTYPE_VOXELSCENECOPYCLEAR_TEMPORALSMOOTHING], threadID);
		}
		else
		{
			device->BindComputePSO(CPSO[CSTYPE_VOXELSCENECOPYCLEAR], threadID);
		}
		device->Dispatch((UINT)(voxelSceneData.res * voxelSceneData.res * voxelSceneData.res / 256), 1, 1, threadID);
		device->EventEnd(threadID);

		if (voxelSceneData.secondaryBounceEnabled)
		{
			device->EventBegin("Voxel Radiance Secondary Bounce", threadID);
			device->UnbindUAVs(1, 1, threadID);
			// Pre-integrate the voxel texture by creating blurred mip levels:
			GenerateMipChain((Texture3D*)textures[TEXTYPE_3D_VOXELRADIANCE], MIPGENFILTER_LINEAR, threadID);
			device->BindUAV(CS, textures[TEXTYPE_3D_VOXELRADIANCE_HELPER], 0, threadID);
			device->BindResource(CS, textures[TEXTYPE_3D_VOXELRADIANCE], 0, threadID);
			device->BindResource(CS, resourceBuffers[RBTYPE_VOXELSCENE], 1, threadID);
			device->BindComputePSO(CPSO[CSTYPE_VOXELRADIANCESECONDARYBOUNCE], threadID);
			device->Dispatch((UINT)(voxelSceneData.res * voxelSceneData.res * voxelSceneData.res / 64), 1, 1, threadID);
			device->EventEnd(threadID);

			device->EventBegin("Voxel Scene Clear Normals", threadID);
			device->UnbindResources(1, 1, threadID);
			device->BindUAV(CS, resourceBuffers[RBTYPE_VOXELSCENE], 0, threadID);
			device->BindComputePSO(CPSO[CSTYPE_VOXELCLEARONLYNORMAL], threadID);
			device->Dispatch((UINT)(voxelSceneData.res * voxelSceneData.res * voxelSceneData.res / 256), 1, 1, threadID);
			device->EventEnd(threadID);

			result = (Texture3D*)textures[TEXTYPE_3D_VOXELRADIANCE_HELPER];
		}

		device->UnbindUAVs(0, 2, threadID);


		// Pre-integrate the voxel texture by creating blurred mip levels:
		{
			GenerateMipChain(result, MIPGENFILTER_LINEAR, threadID);
		}
	}

	if (voxelHelper)
	{
		device->BindResource(VS, result, TEXSLOT_VOXELRADIANCE, threadID);
	}
	device->BindResource(PS, result, TEXSLOT_VOXELRADIANCE, threadID);
	device->BindResource(CS, result, TEXSLOT_VOXELRADIANCE, threadID);

	wiProfiler::GetInstance().EndRange(threadID);
	device->EventEnd(threadID);
}


void wiRenderer::ComputeTiledLightCulling(bool deferred, GRAPHICSTHREAD threadID)
{
	wiProfiler::GetInstance().BeginRange("Tiled Entity Processing", wiProfiler::DOMAIN_GPU, threadID);
	GraphicsDevice* device = wiRenderer::GetDevice();

	int _width = GetInternalResolution().x;
	int _height = GetInternalResolution().y;

	const XMUINT3 tileCount = GetEntityCullingTileCount();

	static int _savedWidth = 0;
	static int _savedHeight = 0;
	bool _resolutionChanged = GetDevice()->ResolutionChanged();
	if (_savedWidth != _width || _savedHeight != _height)
	{
		_resolutionChanged = true;
		_savedWidth = _width;
		_savedHeight = _height;
	}

	static GPUBuffer* frustumBuffer = nullptr;
	if (frustumBuffer == nullptr || _resolutionChanged)
	{
		SAFE_DELETE(frustumBuffer);

		frustumBuffer = new GPUBuffer;

		UINT _stride = sizeof(XMFLOAT4) * 4;

		GPUBufferDesc bd;
		ZeroMemory(&bd, sizeof(bd));
		bd.ByteWidth = _stride * tileCount.x * tileCount.y * tileCount.z; // storing 4 planes for every tile
		bd.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		bd.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.Usage = USAGE_DEFAULT;
		bd.CPUAccessFlags = 0;
		bd.StructureByteStride = _stride;
		device->CreateBuffer(&bd, nullptr, frustumBuffer);
	}
	if (_resolutionChanged)
	{
		SAFE_DELETE(resourceBuffers[RBTYPE_ENTITYINDEXLIST_OPAQUE]);
		SAFE_DELETE(resourceBuffers[RBTYPE_ENTITYINDEXLIST_TRANSPARENT]);
		resourceBuffers[RBTYPE_ENTITYINDEXLIST_OPAQUE] = new GPUBuffer;
		resourceBuffers[RBTYPE_ENTITYINDEXLIST_TRANSPARENT] = new GPUBuffer;

		GPUBufferDesc bd;
		ZeroMemory(&bd, sizeof(bd));
		bd.ByteWidth = sizeof(UINT) * tileCount.x * tileCount.y * tileCount.z * MAX_SHADER_ENTITY_COUNT_PER_TILE;
		bd.Usage = USAGE_DEFAULT;
		bd.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
		bd.CPUAccessFlags = 0;
		bd.StructureByteStride = sizeof(UINT);
		bd.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		device->CreateBuffer(&bd, nullptr, resourceBuffers[RBTYPE_ENTITYINDEXLIST_OPAQUE]);
		device->CreateBuffer(&bd, nullptr, resourceBuffers[RBTYPE_ENTITYINDEXLIST_TRANSPARENT]);
	}
	if (deferred && (textures[TEXTYPE_2D_TILEDDEFERRED_DIFFUSEUAV] == nullptr || textures[TEXTYPE_2D_TILEDDEFERRED_SPECULARUAV] == nullptr))
	{
		TextureDesc desc;
		ZeroMemory(&desc, sizeof(desc));
		desc.ArraySize = 1;
		desc.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = 0;
		desc.Format = RTFormat_deferred_lightbuffer;
		desc.Width = (UINT)_width;
		desc.Height = (UINT)_height;
		desc.MipLevels = 1;
		desc.MiscFlags = 0;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Usage = USAGE_DEFAULT;
		device->CreateTexture2D(&desc, nullptr, (Texture2D**)&textures[TEXTYPE_2D_TILEDDEFERRED_DIFFUSEUAV]);
		device->CreateTexture2D(&desc, nullptr, (Texture2D**)&textures[TEXTYPE_2D_TILEDDEFERRED_SPECULARUAV]);
	}

	// calculate the per-tile frustums once:
	static bool frustumsComplete = false;
	static XMFLOAT4X4 _savedProjection;
	if (memcmp(&_savedProjection, &getCamera()->Projection, sizeof(XMFLOAT4X4)) != 0)
	{
		_savedProjection = getCamera()->Projection;
		frustumsComplete = false;
	}
	if(!frustumsComplete || _resolutionChanged)
	{
		frustumsComplete = true;

		GPUResource* uavs[] = { frustumBuffer };

		device->BindUAVs(CS, uavs, UAVSLOT_TILEFRUSTUMS, ARRAYSIZE(uavs), threadID);
		device->BindComputePSO(CPSO[CSTYPE_TILEFRUSTUMS], threadID);

		DispatchParamsCB dispatchParams;
		dispatchParams.numThreads[0] = tileCount.x;
		dispatchParams.numThreads[1] = tileCount.y;
		dispatchParams.numThreads[2] = 1;
		dispatchParams.numThreadGroups[0] = (UINT)ceilf(dispatchParams.numThreads[0] / (float)TILED_CULLING_BLOCKSIZE);
		dispatchParams.numThreadGroups[1] = (UINT)ceilf(dispatchParams.numThreads[1] / (float)TILED_CULLING_BLOCKSIZE);
		dispatchParams.numThreadGroups[2] = 1;
		device->UpdateBuffer(constantBuffers[CBTYPE_DISPATCHPARAMS], &dispatchParams, threadID);
		device->BindConstantBuffer(CS, constantBuffers[CBTYPE_DISPATCHPARAMS], CB_GETBINDSLOT(DispatchParamsCB), threadID);

		device->Dispatch(dispatchParams.numThreadGroups[0], dispatchParams.numThreadGroups[1], dispatchParams.numThreadGroups[2], threadID);
		device->UnbindUAVs(UAVSLOT_TILEFRUSTUMS, 1, threadID);
		device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);
	}

	if (textures[TEXTYPE_2D_DEBUGUAV] == nullptr || _resolutionChanged)
	{
		SAFE_DELETE(textures[TEXTYPE_2D_DEBUGUAV]);

		TextureDesc desc;
		ZeroMemory(&desc, sizeof(desc));
		desc.Width = (UINT)_width;
		desc.Height = (UINT)_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Usage = USAGE_DEFAULT;
		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;

		device->CreateTexture2D(&desc, nullptr, (Texture2D**)&textures[TEXTYPE_2D_DEBUGUAV]);
	}

	// Perform the culling
	{
		device->EventBegin("Entity Culling", threadID);

		device->UnbindResources(SBSLOT_ENTITYINDEXLIST, 1, threadID);

		device->BindResource(CS, frustumBuffer, SBSLOT_TILEFRUSTUMS, threadID);

		device->BindComputePSO(CPSO_tiledlighting[deferred][GetAdvancedLightCulling()][GetDebugLightCulling()], threadID);

		if (GetDebugLightCulling())
		{
			device->BindUAV(CS, textures[TEXTYPE_2D_DEBUGUAV], UAVSLOT_DEBUGTEXTURE, threadID);
		}


		const FrameCulling& frameCulling = frameCullings[getCamera()];


		DispatchParamsCB dispatchParams;
		dispatchParams.numThreadGroups[0] = tileCount.x;
		dispatchParams.numThreadGroups[1] = tileCount.y;
		dispatchParams.numThreadGroups[2] = 1;
		dispatchParams.numThreads[0] = dispatchParams.numThreadGroups[0] * TILED_CULLING_BLOCKSIZE;
		dispatchParams.numThreads[1] = dispatchParams.numThreadGroups[1] * TILED_CULLING_BLOCKSIZE;
		dispatchParams.numThreads[2] = 1;
		dispatchParams.value0 = (UINT)(frameCulling.culledLights.size() + frameCulling.culledEnvProbes.size() + frameCulling.culledDecals.size());
		device->UpdateBuffer(constantBuffers[CBTYPE_DISPATCHPARAMS], &dispatchParams, threadID);
		device->BindConstantBuffer(CS, constantBuffers[CBTYPE_DISPATCHPARAMS], CB_GETBINDSLOT(DispatchParamsCB), threadID);

		if (deferred)
		{
			GPUResource* uavs[] = {
				textures[TEXTYPE_2D_TILEDDEFERRED_DIFFUSEUAV],
				resourceBuffers[RBTYPE_ENTITYINDEXLIST_TRANSPARENT],
				textures[TEXTYPE_2D_TILEDDEFERRED_SPECULARUAV],
			};
			device->BindUAVs(CS, uavs, UAVSLOT_TILEDDEFERRED_DIFFUSE, ARRAYSIZE(uavs), threadID);

			GetDevice()->BindResource(CS, shadowMapArray_2D, TEXSLOT_SHADOWARRAY_2D, threadID);
			GetDevice()->BindResource(CS, shadowMapArray_Cube, TEXSLOT_SHADOWARRAY_CUBE, threadID);
			GetDevice()->BindResource(CS, shadowMapArray_Transparent, TEXSLOT_SHADOWARRAY_TRANSPARENT, threadID);

			device->Dispatch(dispatchParams.numThreadGroups[0], dispatchParams.numThreadGroups[1], dispatchParams.numThreadGroups[2], threadID);
			device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);
		}
		else
		{
			GPUResource* uavs[] = {
				resourceBuffers[RBTYPE_ENTITYINDEXLIST_OPAQUE],
				resourceBuffers[RBTYPE_ENTITYINDEXLIST_TRANSPARENT],
			};
			device->BindUAVs(CS, uavs, UAVSLOT_ENTITYINDEXLIST_OPAQUE, ARRAYSIZE(uavs), threadID);

			device->Dispatch(dispatchParams.numThreadGroups[0], dispatchParams.numThreadGroups[1], dispatchParams.numThreadGroups[2], threadID);
			device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);
		}

		device->UnbindUAVs(0, 8, threadID); // this unbinds pretty much every uav

		device->EventEnd(threadID);
	}

	wiProfiler::GetInstance().EndRange(threadID);
}
void wiRenderer::ResolveMSAADepthBuffer(Texture2D* dst, Texture2D* src, GRAPHICSTHREAD threadID)
{
	GetDevice()->EventBegin("Resolve MSAA DepthBuffer", threadID);

	GetDevice()->BindResource(CS, src, TEXSLOT_ONDEMAND0, threadID);
	GetDevice()->BindUAV(CS, dst, 0, threadID);

	TextureDesc desc = src->GetDesc();

	GetDevice()->BindComputePSO(CPSO[CSTYPE_RESOLVEMSAADEPTHSTENCIL], threadID);
	GetDevice()->Dispatch((UINT)ceilf(desc.Width / 16.f), (UINT)ceilf(desc.Height / 16.f), 1, threadID);


	GetDevice()->UnbindResources(TEXSLOT_ONDEMAND0, 1, threadID);
	GetDevice()->UnbindUAVs(0, 1, threadID);

	GetDevice()->EventEnd(threadID);
}
void wiRenderer::GenerateMipChain(Texture1D* texture, MIPGENFILTER filter, GRAPHICSTHREAD threadID, int arrayIndex)
{
	assert(0 && "Not implemented!");
}
void wiRenderer::GenerateMipChain(Texture2D* texture, MIPGENFILTER filter, GRAPHICSTHREAD threadID, int arrayIndex)
{
	TextureDesc desc = texture->GetDesc();

	if (desc.MipLevels < 2)
	{
		assert(0);
		return;
	}

	bool hdr = false;
	switch (desc.Format)
	{
	case FORMAT_R16G16B16A16_FLOAT:
	case FORMAT_R32G32B32A32_FLOAT:
		hdr = true;
		break;
	default:
		break;
	}

	GetDevice()->BindRenderTargets(0, nullptr, nullptr, threadID);

	if (desc.MiscFlags & RESOURCE_MISC_TEXTURECUBE)
	{

		if (desc.ArraySize > 6)
		{
			// Cubearray
			assert(arrayIndex >= 0 && "You should only filter a specific cube in the array for now, so provide its index!");

			switch (filter)
			{
			case wiRenderer::MIPGENFILTER_POINT:
				GetDevice()->EventBegin("GenerateMipChain CubeArray - PointFilter", threadID);
				GetDevice()->BindComputePSO(CPSO[hdr ? CSTYPE_GENERATEMIPCHAINCUBEARRAY_FLOAT4_SIMPLEFILTER : CSTYPE_GENERATEMIPCHAINCUBEARRAY_UNORM4_SIMPLEFILTER], threadID);
				GetDevice()->BindSampler(CS, samplers[SSLOT_POINT_CLAMP], SSLOT_ONDEMAND0, threadID);
				break;
			case wiRenderer::MIPGENFILTER_LINEAR:
				GetDevice()->EventBegin("GenerateMipChain CubeArray - LinearFilter", threadID);
				GetDevice()->BindComputePSO(CPSO[hdr ? CSTYPE_GENERATEMIPCHAINCUBEARRAY_FLOAT4_SIMPLEFILTER : CSTYPE_GENERATEMIPCHAINCUBEARRAY_UNORM4_SIMPLEFILTER], threadID);
				GetDevice()->BindSampler(CS, samplers[SSLOT_LINEAR_CLAMP], SSLOT_ONDEMAND0, threadID);
				break;
			case wiRenderer::MIPGENFILTER_LINEAR_MAXIMUM:
				GetDevice()->EventBegin("GenerateMipChain CubeArray - LinearMaxFilter", threadID);
				GetDevice()->BindComputePSO(CPSO[hdr ? CSTYPE_GENERATEMIPCHAINCUBEARRAY_FLOAT4_SIMPLEFILTER : CSTYPE_GENERATEMIPCHAINCUBEARRAY_UNORM4_SIMPLEFILTER], threadID);
				GetDevice()->BindSampler(CS, customsamplers[SSTYPE_MAXIMUM_CLAMP], SSLOT_ONDEMAND0, threadID);
				break;
			default:
				assert(0);
				break;
			}

			for (UINT i = 0; i < desc.MipLevels - 1; ++i)
			{
				GetDevice()->BindUAV(CS, texture, 0, threadID, i + 1);
				GetDevice()->BindResource(CS, texture, TEXSLOT_UNIQUE0, threadID, i);
				desc.Width = max(1, (UINT)ceilf(desc.Width * 0.5f));
				desc.Height = max(1, (UINT)ceilf(desc.Height * 0.5f));

				GenerateMIPChainCB cb;
				cb.outputResolution.x = desc.Width;
				cb.outputResolution.y = desc.Height;
				cb.arrayIndex = arrayIndex;
				GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_MIPGEN], &cb, threadID);
				GetDevice()->BindConstantBuffer(CS, constantBuffers[CBTYPE_MIPGEN], CB_GETBINDSLOT(GenerateMIPChainCB), threadID);

				GetDevice()->Dispatch(
					max(1, (UINT)ceilf((float)desc.Width / GENERATEMIPCHAIN_2D_BLOCK_SIZE)),
					max(1, (UINT)ceilf((float)desc.Height / GENERATEMIPCHAIN_2D_BLOCK_SIZE)),
					6,
					threadID);

				GetDevice()->UAVBarrier((GPUResource**)&texture, 1, threadID);
			}
		}
		else
		{
			// Cubemap
			switch (filter)
			{
			case wiRenderer::MIPGENFILTER_POINT:
				GetDevice()->EventBegin("GenerateMipChain Cube - PointFilter", threadID);
				GetDevice()->BindComputePSO(CPSO[hdr ? CSTYPE_GENERATEMIPCHAINCUBE_FLOAT4_SIMPLEFILTER : CSTYPE_GENERATEMIPCHAINCUBE_UNORM4_SIMPLEFILTER], threadID);
				GetDevice()->BindSampler(CS, samplers[SSLOT_POINT_CLAMP], SSLOT_ONDEMAND0, threadID);
				break;
			case wiRenderer::MIPGENFILTER_LINEAR:
				GetDevice()->EventBegin("GenerateMipChain Cube - LinearFilter", threadID);
				GetDevice()->BindComputePSO(CPSO[hdr ? CSTYPE_GENERATEMIPCHAINCUBE_FLOAT4_SIMPLEFILTER : CSTYPE_GENERATEMIPCHAINCUBE_UNORM4_SIMPLEFILTER], threadID);
				GetDevice()->BindSampler(CS, samplers[SSLOT_LINEAR_CLAMP], SSLOT_ONDEMAND0, threadID);
				break;
			case wiRenderer::MIPGENFILTER_LINEAR_MAXIMUM:
				GetDevice()->EventBegin("GenerateMipChain Cube - LinearMaxFilter", threadID);
				GetDevice()->BindComputePSO(CPSO[hdr ? CSTYPE_GENERATEMIPCHAINCUBE_FLOAT4_SIMPLEFILTER : CSTYPE_GENERATEMIPCHAINCUBE_UNORM4_SIMPLEFILTER], threadID);
				GetDevice()->BindSampler(CS, customsamplers[SSTYPE_MAXIMUM_CLAMP], SSLOT_ONDEMAND0, threadID);
				break;
			default:
				assert(0);
				break;
			}

			for (UINT i = 0; i < desc.MipLevels - 1; ++i)
			{
				GetDevice()->BindUAV(CS, texture, 0, threadID, i + 1);
				GetDevice()->BindResource(CS, texture, TEXSLOT_UNIQUE0, threadID, i);
				desc.Width = max(1, (UINT)ceilf(desc.Width * 0.5f));
				desc.Height = max(1, (UINT)ceilf(desc.Height * 0.5f));

				GenerateMIPChainCB cb;
				cb.outputResolution.x = desc.Width;
				cb.outputResolution.y = desc.Height;
				cb.arrayIndex = 0;
				GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_MIPGEN], &cb, threadID);
				GetDevice()->BindConstantBuffer(CS, constantBuffers[CBTYPE_MIPGEN], CB_GETBINDSLOT(GenerateMIPChainCB), threadID);

				GetDevice()->Dispatch(
					max(1, (UINT)ceilf((float)desc.Width / GENERATEMIPCHAIN_2D_BLOCK_SIZE)),
					max(1, (UINT)ceilf((float)desc.Height / GENERATEMIPCHAIN_2D_BLOCK_SIZE)),
					6,
					threadID);

				GetDevice()->UAVBarrier((GPUResource**)&texture, 1, threadID);
			}
		}

	}
	else
	{
		// Texture2D
		switch (filter)
		{
		case wiRenderer::MIPGENFILTER_POINT:
			GetDevice()->EventBegin("GenerateMipChain 2D - PointFilter", threadID);
			GetDevice()->BindComputePSO(CPSO[hdr ? CSTYPE_GENERATEMIPCHAIN2D_FLOAT4_SIMPLEFILTER : CSTYPE_GENERATEMIPCHAIN2D_UNORM4_SIMPLEFILTER], threadID);
			GetDevice()->BindSampler(CS, samplers[SSLOT_POINT_CLAMP], SSLOT_ONDEMAND0, threadID);
			break;
		case wiRenderer::MIPGENFILTER_LINEAR:
			GetDevice()->EventBegin("GenerateMipChain 2D - LinearFilter", threadID);
			GetDevice()->BindComputePSO(CPSO[hdr ? CSTYPE_GENERATEMIPCHAIN2D_FLOAT4_SIMPLEFILTER : CSTYPE_GENERATEMIPCHAIN2D_UNORM4_SIMPLEFILTER], threadID);
			GetDevice()->BindSampler(CS, samplers[SSLOT_LINEAR_CLAMP], SSLOT_ONDEMAND0, threadID);
			break;
		case wiRenderer::MIPGENFILTER_LINEAR_MAXIMUM:
			GetDevice()->EventBegin("GenerateMipChain 2D - LinearMaxFilter", threadID);
			GetDevice()->BindComputePSO(CPSO[hdr ? CSTYPE_GENERATEMIPCHAIN2D_FLOAT4_SIMPLEFILTER : CSTYPE_GENERATEMIPCHAIN2D_UNORM4_SIMPLEFILTER], threadID);
			GetDevice()->BindSampler(CS, customsamplers[SSTYPE_MAXIMUM_CLAMP], SSLOT_ONDEMAND0, threadID);
			break;
		case wiRenderer::MIPGENFILTER_GAUSSIAN:
			GetDevice()->EventBegin("GenerateMipChain 2D - GaussianFilter", threadID);
			GetDevice()->BindComputePSO(CPSO[hdr ? CSTYPE_GENERATEMIPCHAIN2D_FLOAT4_GAUSSIAN : CSTYPE_GENERATEMIPCHAIN2D_UNORM4_GAUSSIAN], threadID);
			break;
		default:
			assert(0);
			break;
		}

		for (UINT i = 0; i < desc.MipLevels - 1; ++i)
		{
			GetDevice()->BindUAV(CS, texture, 0, threadID, i + 1);
			GetDevice()->BindResource(CS, texture, TEXSLOT_UNIQUE0, threadID, i);
			desc.Width = max(1, (UINT)ceilf(desc.Width * 0.5f));
			desc.Height = max(1, (UINT)ceilf(desc.Height * 0.5f));

			GenerateMIPChainCB cb;
			cb.outputResolution.x = desc.Width;
			cb.outputResolution.y = desc.Height;
			cb.arrayIndex = arrayIndex >= 0 ? (uint)arrayIndex : 0;
			GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_MIPGEN], &cb, threadID);
			GetDevice()->BindConstantBuffer(CS, constantBuffers[CBTYPE_MIPGEN], CB_GETBINDSLOT(GenerateMIPChainCB), threadID);

			GetDevice()->Dispatch(
				max(1, (UINT)ceilf((float)desc.Width / GENERATEMIPCHAIN_2D_BLOCK_SIZE)),
				max(1, (UINT)ceilf((float)desc.Height / GENERATEMIPCHAIN_2D_BLOCK_SIZE)),
				1,
				threadID);

			GetDevice()->UAVBarrier((GPUResource**)&texture, 1, threadID);
		}
	}

	GetDevice()->UnbindResources(TEXSLOT_UNIQUE0, 1, threadID);
	GetDevice()->UnbindUAVs(0, 1, threadID);

	GetDevice()->EventEnd(threadID);
}
void wiRenderer::GenerateMipChain(Texture3D* texture, MIPGENFILTER filter, GRAPHICSTHREAD threadID, int arrayIndex)
{
	TextureDesc desc = texture->GetDesc();

	if (desc.MipLevels < 2)
	{
		assert(0);
		return;
	}

	bool hdr = false;
	switch (desc.Format)
	{
	case FORMAT_R16G16B16A16_FLOAT:
	case FORMAT_R32G32B32A32_FLOAT:
		hdr = true;
		break;
	default:
		break;
	}

	GetDevice()->BindRenderTargets(0, nullptr, nullptr, threadID);

	switch (filter)
	{
	case wiRenderer::MIPGENFILTER_POINT:
		GetDevice()->EventBegin("GenerateMipChain 3D - PointFilter", threadID);
		GetDevice()->BindComputePSO(CPSO[hdr ? CSTYPE_GENERATEMIPCHAIN3D_FLOAT4_SIMPLEFILTER : CSTYPE_GENERATEMIPCHAIN3D_UNORM4_SIMPLEFILTER], threadID);
		GetDevice()->BindSampler(CS, samplers[SSLOT_POINT_CLAMP], SSLOT_ONDEMAND0, threadID);
		break;
	case wiRenderer::MIPGENFILTER_LINEAR:
		GetDevice()->EventBegin("GenerateMipChain 3D - LinearFilter", threadID);
		GetDevice()->BindComputePSO(CPSO[hdr ? CSTYPE_GENERATEMIPCHAIN3D_FLOAT4_SIMPLEFILTER : CSTYPE_GENERATEMIPCHAIN3D_UNORM4_SIMPLEFILTER], threadID);
		GetDevice()->BindSampler(CS, samplers[SSLOT_LINEAR_CLAMP], SSLOT_ONDEMAND0, threadID);
		break;
	case wiRenderer::MIPGENFILTER_LINEAR_MAXIMUM:
		GetDevice()->EventBegin("GenerateMipChain 3D - LinearMaxFilter", threadID);
		GetDevice()->BindComputePSO(CPSO[hdr ? CSTYPE_GENERATEMIPCHAIN3D_FLOAT4_SIMPLEFILTER : CSTYPE_GENERATEMIPCHAIN3D_UNORM4_SIMPLEFILTER], threadID);
		GetDevice()->BindSampler(CS, customsamplers[SSTYPE_MAXIMUM_CLAMP], SSLOT_ONDEMAND0, threadID);
		break;
	case wiRenderer::MIPGENFILTER_GAUSSIAN:
		GetDevice()->EventBegin("GenerateMipChain 3D - GaussianFilter", threadID);
		GetDevice()->BindComputePSO(CPSO[hdr ? CSTYPE_GENERATEMIPCHAIN3D_FLOAT4_GAUSSIAN : CSTYPE_GENERATEMIPCHAIN3D_UNORM4_GAUSSIAN], threadID);
		break;
	}

	for (UINT i = 0; i < desc.MipLevels - 1; ++i)
	{
		GetDevice()->BindUAV(CS, texture, 0, threadID, i + 1);
		GetDevice()->BindResource(CS, texture, TEXSLOT_UNIQUE0, threadID, i);
		desc.Width = max(1, (UINT)ceilf(desc.Width * 0.5f));
		desc.Height = max(1, (UINT)ceilf(desc.Height * 0.5f));
		desc.Depth = max(1, (UINT)ceilf(desc.Depth * 0.5f));

		GenerateMIPChainCB cb;
		cb.outputResolution.x = desc.Width;
		cb.outputResolution.y = desc.Height;
		cb.outputResolution.z = desc.Depth;
		cb.arrayIndex = arrayIndex >= 0 ? (uint)arrayIndex : 0;
		GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_MIPGEN], &cb, threadID);
		GetDevice()->BindConstantBuffer(CS, constantBuffers[CBTYPE_MIPGEN], CB_GETBINDSLOT(GenerateMIPChainCB), threadID);

		GetDevice()->Dispatch(
			max(1, (UINT)ceilf((float)desc.Width / GENERATEMIPCHAIN_3D_BLOCK_SIZE)), 
			max(1, (UINT)ceilf((float)desc.Height / GENERATEMIPCHAIN_3D_BLOCK_SIZE)), 
			max(1, (UINT)ceilf((float)desc.Depth / GENERATEMIPCHAIN_3D_BLOCK_SIZE)), 
			threadID);
	}

	GetDevice()->UnbindResources(TEXSLOT_UNIQUE0, 1, threadID);
	GetDevice()->UnbindUAVs(0, 1, threadID);

	GetDevice()->EventEnd(threadID);
}

void wiRenderer::CopyTexture2D(Texture2D* dst, UINT DstMIP, UINT DstX, UINT DstY, Texture2D* src, UINT SrcMIP, GRAPHICSTHREAD threadID, BORDEREXPANDSTYLE borderExpand)
{
	GraphicsDevice* device = GetDevice();

	const TextureDesc& desc_dst = dst->GetDesc();
	const TextureDesc& desc_src = src->GetDesc();

	assert(desc_dst.BindFlags & BIND_UNORDERED_ACCESS);
	assert(desc_src.BindFlags & BIND_SHADER_RESOURCE);

	device->EventBegin("CopyTexture2D_Region_UNORM4", threadID);

	if (borderExpand == BORDEREXPAND_DISABLE)
	{
		device->BindComputePSO(CPSO[CSTYPE_COPYTEXTURE2D_UNORM4], threadID);
	}
	else
	{
		device->BindComputePSO(CPSO[CSTYPE_COPYTEXTURE2D_UNORM4_BORDEREXPAND], threadID);
	}

	CopyTextureCB cb;
	cb.xCopyDest.x = DstX;
	cb.xCopyDest.y = DstY;
	cb.xCopySrcSize.x = desc_src.Width >> SrcMIP;
	cb.xCopySrcSize.y = desc_src.Height >> SrcMIP;
	cb.xCopySrcMIP = SrcMIP;
	cb.xCopyBorderExpandStyle = (uint)borderExpand;
	device->UpdateBuffer(constantBuffers[CBTYPE_COPYTEXTURE], &cb, threadID);

	device->BindConstantBuffer(CS, constantBuffers[CBTYPE_COPYTEXTURE], CB_GETBINDSLOT(CopyTextureCB), threadID);

	device->BindResource(CS, src, TEXSLOT_ONDEMAND0, threadID);

	if (DstMIP > 0)
	{
		assert(desc_dst.MipLevels > DstMIP);
		device->BindUAV(CS, dst, 0, threadID, DstMIP);
	}
	else
	{
		device->BindUAV(CS, dst, 0, threadID);
	}

	device->Dispatch((UINT)ceilf((float)cb.xCopySrcSize.x / 8.0f), (UINT)ceilf((float)cb.xCopySrcSize.y / 8.0f), 1, threadID);

	device->UnbindUAVs(0, 1, threadID);

	device->EventEnd(threadID);
}



// Should I care that usually buffers are not declared here?
static GPUBuffer* bvhNodeBuffer = nullptr;
static GPUBuffer* bvhAABBBuffer = nullptr;
static GPUBuffer* bvhFlagBuffer = nullptr;
static GPUBuffer* triangleBuffer = nullptr;
static GPUBuffer* clusterCounterBuffer = nullptr;
static GPUBuffer* clusterIndexBuffer = nullptr;
static GPUBuffer* clusterMortonBuffer = nullptr;
static GPUBuffer* clusterSortedMortonBuffer = nullptr;
static GPUBuffer* clusterOffsetBuffer = nullptr;
static GPUBuffer* clusterAABBBuffer = nullptr;
static GPUBuffer* clusterConeBuffer = nullptr;
void wiRenderer::BuildSceneBVH(GRAPHICSTHREAD threadID)
{
	GraphicsDevice* device = GetDevice();
	Scene& scene = GetScene();

	// Pre-gather scene properties:
	uint totalTriangles = 0;
	for (size_t i = 0; i < scene.meshes.GetCount(); ++i)
	{
		const MeshComponent& mesh = scene.meshes[i];

		totalTriangles += (uint)mesh.indices.size() / 3;
	}

	static uint maxTriangleCount = 0;
	static uint maxClusterCount = 0;

	if (totalTriangles > maxTriangleCount)
	{
		maxTriangleCount = totalTriangles;
		maxClusterCount = maxTriangleCount; // todo: cluster / triangle capacity

		GPUBufferDesc desc;
		HRESULT hr;

		SAFE_DELETE(bvhNodeBuffer);
		SAFE_DELETE(bvhAABBBuffer);
		SAFE_DELETE(bvhFlagBuffer);
		SAFE_DELETE(triangleBuffer);
		SAFE_DELETE(clusterCounterBuffer);
		SAFE_DELETE(clusterIndexBuffer);
		SAFE_DELETE(clusterMortonBuffer);
		SAFE_DELETE(clusterSortedMortonBuffer);
		SAFE_DELETE(clusterOffsetBuffer);
		SAFE_DELETE(clusterAABBBuffer);
		SAFE_DELETE(clusterConeBuffer);
		bvhNodeBuffer = new GPUBuffer;
		bvhAABBBuffer = new GPUBuffer;
		bvhFlagBuffer = new GPUBuffer;
		triangleBuffer = new GPUBuffer;
		clusterCounterBuffer = new GPUBuffer;
		clusterIndexBuffer = new GPUBuffer;
		clusterMortonBuffer = new GPUBuffer;
		clusterSortedMortonBuffer = new GPUBuffer;
		clusterOffsetBuffer = new GPUBuffer;
		clusterAABBBuffer = new GPUBuffer;
		clusterConeBuffer = new GPUBuffer;

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(BVHNode);
		desc.ByteWidth = desc.StructureByteStride * maxClusterCount * 2;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		hr = device->CreateBuffer(&desc, nullptr, bvhNodeBuffer);
		assert(SUCCEEDED(hr));
		device->SetName(bvhNodeBuffer, "BVHNodeBuffer");

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(BVHAABB);
		desc.ByteWidth = desc.StructureByteStride * maxClusterCount * 2;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		hr = device->CreateBuffer(&desc, nullptr, bvhAABBBuffer);
		assert(SUCCEEDED(hr));
		device->SetName(bvhAABBBuffer, "BVHAABBBuffer");

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(uint);
		desc.ByteWidth = desc.StructureByteStride * (maxClusterCount - 1); // only for internal nodes
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		hr = device->CreateBuffer(&desc, nullptr, bvhFlagBuffer);
		assert(SUCCEEDED(hr));
		device->SetName(bvhFlagBuffer, "BVHFlagBuffer");

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(BVHMeshTriangle);
		desc.ByteWidth = desc.StructureByteStride * maxTriangleCount;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		hr = device->CreateBuffer(&desc, nullptr, triangleBuffer);
		assert(SUCCEEDED(hr));
		device->SetName(triangleBuffer, "BVHTriangleBuffer");

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(uint);
		desc.ByteWidth = desc.StructureByteStride;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		desc.Usage = USAGE_DEFAULT;
		hr = device->CreateBuffer(&desc, nullptr, clusterCounterBuffer);
		assert(SUCCEEDED(hr));
		device->SetName(clusterCounterBuffer, "BVHClusterCounterBuffer");

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(uint);
		desc.ByteWidth = desc.StructureByteStride * maxClusterCount;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		hr = device->CreateBuffer(&desc, nullptr, clusterIndexBuffer);
		assert(SUCCEEDED(hr));
		device->SetName(clusterIndexBuffer, "BVHClusterIndexBuffer");

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(uint);
		desc.ByteWidth = desc.StructureByteStride * maxClusterCount;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		hr = device->CreateBuffer(&desc, nullptr, clusterMortonBuffer);
		hr = device->CreateBuffer(&desc, nullptr, clusterSortedMortonBuffer);
		assert(SUCCEEDED(hr));
		device->SetName(clusterMortonBuffer, "BVHClusterMortonBuffer");
		device->SetName(clusterSortedMortonBuffer, "BVHSortedClusterMortonBuffer");

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(uint2);
		desc.ByteWidth = desc.StructureByteStride * maxClusterCount;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		hr = device->CreateBuffer(&desc, nullptr, clusterOffsetBuffer);
		assert(SUCCEEDED(hr));
		device->SetName(clusterOffsetBuffer, "BVHClusterOffsetBuffer");

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(BVHAABB);
		desc.ByteWidth = desc.StructureByteStride * maxClusterCount;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		hr = device->CreateBuffer(&desc, nullptr, clusterAABBBuffer);
		assert(SUCCEEDED(hr));
		device->SetName(clusterAABBBuffer, "BVHClusterAABBBuffer");

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(ClusterCone);
		desc.ByteWidth = desc.StructureByteStride * maxClusterCount;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		hr = device->CreateBuffer(&desc, nullptr, clusterConeBuffer);
		assert(SUCCEEDED(hr));
		device->SetName(clusterConeBuffer, "BVHClusterConeBuffer");
	}

	static GPUBuffer* indirectBuffer = nullptr; // GPU job kicks
	if (indirectBuffer == nullptr)
	{
		GPUBufferDesc desc;
		HRESULT hr;

		SAFE_DELETE(indirectBuffer);
		indirectBuffer = new GPUBuffer;

		desc.BindFlags = BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(IndirectDispatchArgs) * 2;
		desc.ByteWidth = desc.StructureByteStride;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_DRAWINDIRECT_ARGS | RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		desc.Usage = USAGE_DEFAULT;
		hr = device->CreateBuffer(&desc, nullptr, indirectBuffer);
		assert(SUCCEEDED(hr));
	}


	wiProfiler::GetInstance().BeginRange("BVH Rebuild", wiProfiler::DOMAIN_GPU, threadID);

	device->EventBegin("BVH - Reset", threadID);
	{
		device->BindComputePSO(CPSO[CSTYPE_BVH_RESET], threadID);

		GPUResource* uavs[] = {
			clusterCounterBuffer,
			bvhNodeBuffer,
			bvhAABBBuffer,
		};
		device->BindUAVs(CS, uavs, 0, ARRAYSIZE(uavs), threadID);

		device->Dispatch(1, 1, 1, threadID);
	}
	device->EventEnd(threadID);


	uint32_t triangleCount = 0;
	uint32_t materialCount = 0;

	device->EventBegin("BVH - Classification", threadID);
	{
		device->BindComputePSO(CPSO[CSTYPE_BVH_CLASSIFICATION], threadID);
		GPUResource* uavs[] = {
			triangleBuffer,
			clusterCounterBuffer,
			clusterIndexBuffer,
			clusterMortonBuffer,
			clusterOffsetBuffer,
			clusterAABBBuffer,
		};
		device->BindUAVs(CS, uavs, 0, ARRAYSIZE(uavs), threadID);

		for (size_t i = 0; i < scene.objects.GetCount(); ++i)
		{
			const ObjectComponent& object = scene.objects[i];

			if (object.meshID != INVALID_ENTITY)
			{
				const MeshComponent& mesh = *scene.meshes.GetComponent(object.meshID);
				Entity entity = scene.objects.GetEntity(i);
				const TransformComponent& transform = *scene.transforms.GetComponent(entity);

				BVHCB cb;
				cb.xTraceBVHWorld = transform.world;
				cb.xTraceBVHMaterialOffset = materialCount;
				cb.xTraceBVHMeshTriangleOffset = triangleCount;
				cb.xTraceBVHMeshTriangleCount = (uint)mesh.indices.size() / 3;
				cb.xTraceBVHMeshVertexPOSStride = sizeof(MeshComponent::Vertex_POS);

				device->UpdateBuffer(constantBuffers[CBTYPE_BVH], &cb, threadID);

				triangleCount += cb.xTraceBVHMeshTriangleCount;

				device->BindConstantBuffer(CS, constantBuffers[CBTYPE_BVH], CB_GETBINDSLOT(BVHCB), threadID);

				GPUResource* res[] = {
					mesh.indexBuffer.get(),
					mesh.vertexBuffer_POS.get(),
					mesh.vertexBuffer_TEX.get(),
				};
				device->BindResources(CS, res, TEXSLOT_ONDEMAND0, ARRAYSIZE(res), threadID);

				device->Dispatch((UINT)ceilf((float)cb.xTraceBVHMeshTriangleCount / (float)BVH_CLASSIFICATION_GROUPSIZE), 1, 1, threadID);

				for (auto& subset : mesh.subsets)
				{
					materialCount++;
				}
			}
		}

		device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);
		device->UnbindUAVs(0, ARRAYSIZE(uavs), threadID);
	}
	device->EventEnd(threadID);


	device->EventBegin("BVH - Sort Cluster Mortons", threadID);
	wiGPUSortLib::Sort(maxClusterCount, clusterMortonBuffer, clusterCounterBuffer, 0, clusterIndexBuffer, threadID);
	device->EventEnd(threadID);

	device->EventBegin("BVH - Kick Jobs", threadID);
	{
		device->BindComputePSO(CPSO[CSTYPE_BVH_KICKJOBS], threadID);
		GPUResource* uavs[] = {
			indirectBuffer,
		};
		device->BindUAVs(CS, uavs, 0, ARRAYSIZE(uavs), threadID);

		GPUResource* res[] = {
			clusterCounterBuffer,
		};
		device->BindResources(CS, res, TEXSLOT_ONDEMAND0, ARRAYSIZE(res), threadID);

		device->Dispatch(1, 1, 1, threadID);

		device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);
		device->UnbindUAVs(0, ARRAYSIZE(uavs), threadID);
	}
	device->EventEnd(threadID);

	device->EventBegin("BVH - Cluster Processor", threadID);
	{
		device->BindComputePSO(CPSO[CSTYPE_BVH_CLUSTERPROCESSOR], threadID);
		GPUResource* uavs[] = {
			clusterSortedMortonBuffer,
			clusterConeBuffer,
		};
		device->BindUAVs(CS, uavs, 0, ARRAYSIZE(uavs), threadID);

		GPUResource* res[] = {
			clusterCounterBuffer,
			clusterIndexBuffer,
			clusterMortonBuffer,
			clusterOffsetBuffer,
			clusterAABBBuffer,
			triangleBuffer,
		};
		device->BindResources(CS, res, TEXSLOT_ONDEMAND0, ARRAYSIZE(res), threadID);

		device->DispatchIndirect(indirectBuffer, ARGUMENTBUFFER_OFFSET_CLUSTERPROCESSOR, threadID);


		device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);
		device->UnbindUAVs(0, ARRAYSIZE(uavs), threadID);
	}
	device->EventEnd(threadID);

	device->EventBegin("BVH - Build Hierarchy", threadID);
	{
		device->BindComputePSO(CPSO[CSTYPE_BVH_HIERARCHY], threadID);
		GPUResource* uavs[] = {
			bvhNodeBuffer,
			bvhFlagBuffer,
		};
		device->BindUAVs(CS, uavs, 0, ARRAYSIZE(uavs), threadID);

		GPUResource* res[] = {
			clusterCounterBuffer,
			clusterSortedMortonBuffer,
		};
		device->BindResources(CS, res, TEXSLOT_ONDEMAND0, ARRAYSIZE(res), threadID);

		device->DispatchIndirect(indirectBuffer, ARGUMENTBUFFER_OFFSET_HIERARCHY, threadID);


		device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);
		device->UnbindUAVs(0, ARRAYSIZE(uavs), threadID);
	}
	device->EventEnd(threadID);

	device->EventBegin("BVH - Propagate AABB", threadID);
	{
		device->BindComputePSO(CPSO[CSTYPE_BVH_PROPAGATEAABB], threadID);
		GPUResource* uavs[] = {
			bvhAABBBuffer,
			bvhFlagBuffer,
		};
		device->BindUAVs(CS, uavs, 0, ARRAYSIZE(uavs), threadID);

		GPUResource* res[] = {
			clusterCounterBuffer,
			clusterIndexBuffer,
			clusterAABBBuffer,
			bvhNodeBuffer,
		};
		device->BindResources(CS, res, TEXSLOT_ONDEMAND0, ARRAYSIZE(res), threadID);

		device->DispatchIndirect(indirectBuffer, ARGUMENTBUFFER_OFFSET_CLUSTERPROCESSOR, threadID);


		device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);
		device->UnbindUAVs(0, ARRAYSIZE(uavs), threadID);
	}
	device->EventEnd(threadID);

	wiProfiler::GetInstance().EndRange(threadID); // BVH rebuild
}

void wiRenderer::DrawTracedScene(CameraComponent* camera, Texture2D* result, GRAPHICSTHREAD threadID)
{
	GraphicsDevice* device = wiRenderer::GetDevice();
	Scene& scene = GetScene();

	device->EventBegin("DrawTracedScene", threadID);

	uint _width = GetInternalResolution().x;
	uint _height = GetInternalResolution().y;

	// Ray storage buffer:
	static GPUBuffer* rayBuffer[2] = {};
	static uint RayCountPrev = 0;
	const uint _raycount = _width * _height;

	if (RayCountPrev != _raycount || rayBuffer[0] == nullptr || rayBuffer[1] == nullptr)
	{
		GPUBufferDesc desc;
		HRESULT hr;

		SAFE_DELETE(rayBuffer[0]);
		SAFE_DELETE(rayBuffer[1]);
		rayBuffer[0] = new GPUBuffer;
		rayBuffer[1] = new GPUBuffer;

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(TracedRenderingStoredRay);
		desc.ByteWidth = desc.StructureByteStride * _raycount;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		hr = device->CreateBuffer(&desc, nullptr, rayBuffer[0]);
		hr = device->CreateBuffer(&desc, nullptr, rayBuffer[1]);
		assert(SUCCEEDED(hr));

		RayCountPrev = _raycount;
	}

	// Misc buffers:
	static GPUBuffer* indirectBuffer = nullptr; // GPU job kicks
	static GPUBuffer* counterBuffer[2] = {}; // Active ray counter

	if (indirectBuffer == nullptr || counterBuffer == nullptr)
	{
		GPUBufferDesc desc;
		HRESULT hr;

		SAFE_DELETE(indirectBuffer);
		indirectBuffer = new GPUBuffer;

		desc.BindFlags = BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(IndirectDispatchArgs);
		desc.ByteWidth = desc.StructureByteStride;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_DRAWINDIRECT_ARGS | RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		desc.Usage = USAGE_DEFAULT;
		hr = device->CreateBuffer(&desc, nullptr, indirectBuffer);
		assert(SUCCEEDED(hr));


		SAFE_DELETE(counterBuffer[0]);
		SAFE_DELETE(counterBuffer[1]);
		counterBuffer[0] = new GPUBuffer;
		counterBuffer[1] = new GPUBuffer;

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.StructureByteStride = sizeof(uint);
		desc.ByteWidth = desc.StructureByteStride;
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		desc.Usage = USAGE_DEFAULT;
		hr = device->CreateBuffer(&desc, nullptr, counterBuffer[0]);
		hr = device->CreateBuffer(&desc, nullptr, counterBuffer[1]);
		assert(SUCCEEDED(hr));
	}

	// Traced Scene Texture Atlas:
	static Texture2D* atlasTexture = nullptr;
	using namespace wiRectPacker;
	static unordered_set<Texture2D*> sceneTextures;
	if (sceneTextures.empty())
	{
		sceneTextures.insert(wiTextureHelper::getInstance()->getWhite());
		sceneTextures.insert(wiTextureHelper::getInstance()->getNormalMapDefault());
	}

	for (size_t i = 0; i < scene.objects.GetCount(); ++i)
	{
		const ObjectComponent& object = scene.objects[i];

		if (object.meshID != INVALID_ENTITY)
		{
			const MeshComponent& mesh = *scene.meshes.GetComponent(object.meshID);

			for (auto& subset : mesh.subsets)
			{
				const MaterialComponent& material = *scene.materials.GetComponent(subset.materialID);

				sceneTextures.insert(material.GetBaseColorMap());
				sceneTextures.insert(material.GetSurfaceMap());
				sceneTextures.insert(material.GetNormalMap());
			}
		}

	}

	bool repackAtlas = false;
	static unordered_map<Texture2D*, rect_xywhf> storedTextures;
	const int atlasWrapBorder = 1;
	for (Texture2D* tex : sceneTextures)
	{
		if (tex == nullptr)
		{
			continue;
		}

		if (storedTextures.find(tex) == storedTextures.end())
		{
			// we need to pack this texture into the atlas
			rect_xywhf newRect = rect_xywhf(0, 0, tex->GetDesc().Width + atlasWrapBorder * 2, tex->GetDesc().Height + atlasWrapBorder * 2);
			storedTextures[tex] = newRect;

			repackAtlas = true;
		}

	}

	if (repackAtlas)
	{
		rect_xywhf** out_rects = new rect_xywhf*[storedTextures.size()];
		int i = 0;
		for (auto& it : storedTextures)
		{
			out_rects[i] = &it.second;
			i++;
		}

		std::vector<bin> bins;
		if (pack(out_rects, (int)storedTextures.size(), 16384, bins))
		{
			assert(bins.size() == 1 && "The regions won't fit into the texture!");

			SAFE_DELETE(atlasTexture);

			TextureDesc desc;
			ZeroMemory(&desc, sizeof(desc));
			desc.Width = (UINT)bins[0].size.w;
			desc.Height = (UINT)bins[0].size.h;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = USAGE_DEFAULT;
			desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags = 0;

			device->CreateTexture2D(&desc, nullptr, &atlasTexture);

			for (auto& it : storedTextures)
			{
				CopyTexture2D(atlasTexture, 0, it.second.x + atlasWrapBorder, it.second.y + atlasWrapBorder, it.first, 0, threadID, BORDEREXPAND_WRAP);
			}
		}
		else
		{
			wiBackLog::post("Tracing atlas packing failed!");
		}

		SAFE_DELETE_ARRAY(out_rects);
	}

	static TracedRenderingMaterial materialArray[1000] = {}; // todo realloc!
	static GPUBuffer* materialBuffer = nullptr;

	if (materialBuffer == nullptr)
	{
		GPUBufferDesc desc;
		HRESULT hr;

		SAFE_DELETE(materialBuffer);
		materialBuffer = new GPUBuffer;

		desc.BindFlags = BIND_SHADER_RESOURCE;
		desc.StructureByteStride = sizeof(TracedRenderingMaterial);
		desc.ByteWidth = desc.StructureByteStride * ARRAYSIZE(materialArray);
		desc.CPUAccessFlags = 0;
		desc.Format = FORMAT_UNKNOWN;
		desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.Usage = USAGE_DEFAULT;
		hr = device->CreateBuffer(&desc, nullptr, materialBuffer);
		assert(SUCCEEDED(hr));
	}

	// Pre-gather scene properties:
	uint32_t totalTriangles = 0;
	uint32_t totalMaterials = 0;
	for (size_t i = 0; i < scene.objects.GetCount(); ++i)
	{
		const ObjectComponent& object = scene.objects[i];

		if (object.meshID != INVALID_ENTITY)
		{
			const MeshComponent& mesh = *scene.meshes.GetComponent(object.meshID);

			totalTriangles += (uint)mesh.indices.size() / 3;

			for (auto& subset : mesh.subsets)
			{
				const MaterialComponent& material = *scene.materials.GetComponent(subset.materialID);

				MaterialCB mat;
				mat.Create(material);

				// Copy base params:
				materialArray[totalMaterials].baseColor = mat.baseColor;
				materialArray[totalMaterials].texMulAdd = mat.texMulAdd;
				materialArray[totalMaterials].roughness = mat.roughness;
				materialArray[totalMaterials].reflectance = mat.reflectance;
				materialArray[totalMaterials].metalness = mat.metalness;
				materialArray[totalMaterials].emissive = mat.emissive;
				materialArray[totalMaterials].refractionIndex = mat.refractionIndex;
				materialArray[totalMaterials].subsurfaceScattering = mat.subsurfaceScattering;
				materialArray[totalMaterials].normalMapStrength = mat.normalMapStrength;
				materialArray[totalMaterials].parallaxOcclusionMapping = mat.normalMapStrength;

				// Add extended properties:
				const TextureDesc& desc = atlasTexture->GetDesc();
				rect_xywhf rect;


				if (material.GetBaseColorMap() != nullptr)
				{
					rect = storedTextures[material.GetBaseColorMap()];
				}
				else
				{
					rect = storedTextures[wiTextureHelper::getInstance()->getWhite()];
				}
				// eliminate border expansion:
				rect.x += atlasWrapBorder;
				rect.y += atlasWrapBorder;
				rect.w -= atlasWrapBorder * 2;
				rect.h -= atlasWrapBorder * 2;
				materialArray[totalMaterials].baseColorAtlasMulAdd = XMFLOAT4((float)rect.w / (float)desc.Width, (float)rect.h / (float)desc.Height,
					(float)rect.x / (float)desc.Width, (float)rect.y / (float)desc.Height);



				if (material.GetSurfaceMap() != nullptr)
				{
					rect = storedTextures[material.GetSurfaceMap()];
				}
				else
				{
					rect = storedTextures[wiTextureHelper::getInstance()->getWhite()];
				}
				// eliminate border expansion:
				rect.x += atlasWrapBorder;
				rect.y += atlasWrapBorder;
				rect.w -= atlasWrapBorder * 2;
				rect.h -= atlasWrapBorder * 2;
				materialArray[totalMaterials].surfaceMapAtlasMulAdd = XMFLOAT4((float)rect.w / (float)desc.Width, (float)rect.h / (float)desc.Height,
					(float)rect.x / (float)desc.Width, (float)rect.y / (float)desc.Height);



				if (material.GetNormalMap() != nullptr)
				{
					rect = storedTextures[material.GetNormalMap()];
				}
				else
				{

					rect = storedTextures[wiTextureHelper::getInstance()->getNormalMapDefault()];
				}
				// eliminate border expansion:
				rect.x += atlasWrapBorder;
				rect.y += atlasWrapBorder;
				rect.w -= atlasWrapBorder * 2;
				rect.h -= atlasWrapBorder * 2;
				materialArray[totalMaterials].normalMapAtlasMulAdd = XMFLOAT4((float)rect.w / (float)desc.Width, (float)rect.h / (float)desc.Height,
					(float)rect.x / (float)desc.Width, (float)rect.y / (float)desc.Height);


				totalMaterials++;
			}
		}
	}
	device->UpdateBuffer(materialBuffer, materialArray, threadID, sizeof(TracedRenderingMaterial) * totalMaterials);


	// Begin raytrace

	wiProfiler::GetInstance().BeginRange("RayTrace - ALL", wiProfiler::DOMAIN_GPU, threadID);

	const XMFLOAT4& halton = wiMath::GetHaltonSequence((int)GetDevice()->GetFrameCount());
	TracedRenderingCB cb;
	cb.xTracePixelOffset = XMFLOAT2(halton.x, halton.y);
	cb.xTraceRandomSeed = renderTime;
	cb.xTraceMeshTriangleCount = totalTriangles;

	device->UpdateBuffer(constantBuffers[CBTYPE_RAYTRACE], &cb, threadID);

	device->EventBegin("Clear", threadID);
	{
		device->BindComputePSO(CPSO[CSTYPE_RAYTRACE_CLEAR], threadID);

		device->BindConstantBuffer(CS, constantBuffers[CBTYPE_RAYTRACE], CB_GETBINDSLOT(TracedRenderingCB), threadID);

		GPUResource* uavs[] = {
			result,
		};
		device->BindUAVs(CS, uavs, 0, ARRAYSIZE(uavs), threadID);

		device->Dispatch((UINT)ceilf((float)_width / (float)TRACEDRENDERING_CLEAR_BLOCKSIZE), (UINT)ceilf((float)_height / (float)TRACEDRENDERING_CLEAR_BLOCKSIZE), 1, threadID);
		device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);

		device->UnbindUAVs(0, ARRAYSIZE(uavs), threadID);
	}
	device->EventEnd(threadID);

	device->EventBegin("Launch Rays", threadID);
	{
		device->BindComputePSO(CPSO[CSTYPE_RAYTRACE_LAUNCH], threadID);

		device->BindConstantBuffer(CS, constantBuffers[CBTYPE_RAYTRACE], CB_GETBINDSLOT(TracedRenderingCB), threadID);

		GPUResource* uavs[] = {
			rayBuffer[0],
		};
		device->BindUAVs(CS, uavs, 0, ARRAYSIZE(uavs), threadID);

		device->Dispatch((UINT)ceilf((float)_width / (float)TRACEDRENDERING_LAUNCH_BLOCKSIZE), (UINT)ceilf((float)_height / (float)TRACEDRENDERING_LAUNCH_BLOCKSIZE), 1, threadID);
		device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);

		device->UnbindUAVs(0, ARRAYSIZE(uavs), threadID);

		// just write initial ray count:
		device->UpdateBuffer(counterBuffer[0], &_raycount, threadID);
	}
	device->EventEnd(threadID);



	// Set up tracing resources:
	GPUResource* res[] = {
		materialBuffer,
		triangleBuffer,
		clusterCounterBuffer,
		clusterIndexBuffer,
		clusterOffsetBuffer,
		clusterConeBuffer,
		bvhNodeBuffer,
		bvhAABBBuffer,
	};
	device->BindResources(CS, res, TEXSLOT_ONDEMAND0, ARRAYSIZE(res), threadID);

	if (atlasTexture != nullptr)
	{
		device->BindResource(CS, atlasTexture, TEXSLOT_ONDEMAND8, threadID);
	}
	else
	{
		device->BindResource(CS, wiTextureHelper::getInstance()->getWhite(), TEXSLOT_ONDEMAND8, threadID);
	}

	for (int bounce = 0; bounce < 8; ++bounce)
	{
		const int __readBufferID = bounce % 2;
		const int __writeBufferID = (bounce + 1) % 2;

		cb.xTraceRandomSeed = renderTime + (float)bounce;
		device->UpdateBuffer(constantBuffers[CBTYPE_RAYTRACE], &cb, threadID);
		device->BindConstantBuffer(CS, constantBuffers[CBTYPE_RAYTRACE], CB_GETBINDSLOT(TracedRenderingCB), threadID);


		// 1.) Kick off raytracing jobs for this bounce
		device->EventBegin("Kick Raytrace Jobs", threadID);
		{
			// Prepare indirect dispatch based on counter buffer value:
			device->BindComputePSO(CPSO[CSTYPE_RAYTRACE_KICKJOBS], threadID);

			GPUResource* res[] = {
				counterBuffer[__readBufferID],
			};
			device->BindResources(CS, res, TEXSLOT_UNIQUE0, ARRAYSIZE(res), threadID);
			GPUResource* uavs[] = {
				counterBuffer[__writeBufferID],
				indirectBuffer,
			};
			device->BindUAVs(CS, uavs, 0, ARRAYSIZE(uavs), threadID);

			device->Dispatch(1, 1, 1, threadID);

			device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);
			device->UnbindUAVs(0, ARRAYSIZE(uavs), threadID);
		}
		device->EventEnd(threadID);

		if (bounce > 0)
		{
			if (bounce == 1)
			{
				wiProfiler::GetInstance().BeginRange("RayTrace - First Light Sampling", wiProfiler::DOMAIN_GPU, threadID);
			}

			// 2.) Light sampling (any hit) <- only after first bounce has occured
			device->EventBegin("Light Sampling Rays", threadID);
			{
				// Indirect dispatch on active rays:
				device->BindComputePSO(CPSO[CSTYPE_RAYTRACE_LIGHTSAMPLING], threadID);

				GPUResource* res[] = {
					counterBuffer[__readBufferID],
					rayBuffer[__readBufferID],
				};
				device->BindResources(CS, res, TEXSLOT_UNIQUE0, ARRAYSIZE(res), threadID);
				GPUResource* uavs[] = {
					result,
				};
				device->BindUAVs(CS, uavs, 0, ARRAYSIZE(uavs), threadID);

				device->DispatchIndirect(indirectBuffer, 0, threadID);

				device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);
				device->UnbindUAVs(0, ARRAYSIZE(uavs), threadID);
			}
			device->EventEnd(threadID);

			if (bounce == 1)
			{
				wiProfiler::GetInstance().EndRange(threadID); // RayTrace - First Light Sampling
			}
		}

		if (bounce == 0)
		{
			wiProfiler::GetInstance().BeginRange("RayTrace - First Bounce", wiProfiler::DOMAIN_GPU, threadID);
		}

		// 3.) Compute Primary Trace (closest hit)
		device->EventBegin("Primary Rays Bounce", threadID);
		{
			// Indirect dispatch on active rays:
			device->BindComputePSO(CPSO[CSTYPE_RAYTRACE_PRIMARY], threadID);

			GPUResource* res[] = {
				counterBuffer[__readBufferID],
				rayBuffer[__readBufferID],
			};
			device->BindResources(CS, res, TEXSLOT_UNIQUE0, ARRAYSIZE(res), threadID);
			GPUResource* uavs[] = {
				counterBuffer[__writeBufferID],
				rayBuffer[__writeBufferID],
				result,
			};
			device->BindUAVs(CS, uavs, 0, ARRAYSIZE(uavs), threadID);

			device->DispatchIndirect(indirectBuffer, 0, threadID);

			device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);
			device->UnbindUAVs(0, ARRAYSIZE(uavs), threadID);
		}
		device->EventEnd(threadID);

		if (bounce == 0)
		{
			wiProfiler::GetInstance().EndRange(threadID); // RayTrace - First Bounce
		}

	}

	wiProfiler::GetInstance().EndRange(threadID); // RayTrace - ALL




	device->EventEnd(threadID); // DrawTracedScene
}

void wiRenderer::GenerateClouds(Texture2D* dst, UINT refinementCount, float randomness, GRAPHICSTHREAD threadID)
{
	GetDevice()->EventBegin("Cloud Generator", threadID);

	TextureDesc src_desc = wiTextureHelper::getInstance()->getRandom64x64()->GetDesc();

	TextureDesc dst_desc = dst->GetDesc();
	assert(dst_desc.BindFlags & BIND_UNORDERED_ACCESS);

	GetDevice()->BindResource(CS, wiTextureHelper::getInstance()->getRandom64x64(), TEXSLOT_ONDEMAND0, threadID);
	GetDevice()->BindUAV(CS, dst, 0, threadID);

	CloudGeneratorCB cb;
	cb.xNoiseTexDim = XMFLOAT2((float)src_desc.Width, (float)src_desc.Height);
	cb.xRandomness = randomness;
	if (refinementCount == 0)
	{
		cb.xRefinementCount = max(1, (UINT)log2(dst_desc.Width));
	}
	else
	{
		cb.xRefinementCount = refinementCount;
	}
	GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_CLOUDGENERATOR], &cb, threadID);
	GetDevice()->BindConstantBuffer(CS, constantBuffers[CBTYPE_CLOUDGENERATOR], CB_GETBINDSLOT(CloudGeneratorCB), threadID);

	GetDevice()->BindComputePSO(CPSO[CSTYPE_CLOUDGENERATOR], threadID);
	GetDevice()->Dispatch((UINT)ceilf(dst_desc.Width / (float)CLOUDGENERATOR_BLOCKSIZE), (UINT)ceilf(dst_desc.Height / (float)CLOUDGENERATOR_BLOCKSIZE), 1, threadID);

	GetDevice()->UnbindResources(TEXSLOT_ONDEMAND0, 1, threadID);
	GetDevice()->UnbindUAVs(0, 1, threadID);


	GetDevice()->EventEnd(threadID);
}

void wiRenderer::ManageDecalAtlas(GRAPHICSTHREAD threadID)
{
	GraphicsDevice* device = GetDevice();


	static Texture2D* atlasTexture = nullptr;
	bool repackAtlas = false;
	const int atlasClampBorder = 1;

	using namespace wiRectPacker;
	static unordered_map<Texture2D*, rect_xywhf> storedTextures;

	Scene& scene = GetScene();

	// Gather all decal textures:
	for (size_t i = 0; i < scene.decals.GetCount(); ++i)
	{
		const DecalComponent& decal = scene.decals[i];

		if (decal.texture != nullptr)
		{
			if (storedTextures.find(decal.texture) == storedTextures.end())
			{
				// we need to pack this decal texture into the atlas
				rect_xywhf newRect = rect_xywhf(0, 0, decal.texture->GetDesc().Width + atlasClampBorder * 2, decal.texture->GetDesc().Height + atlasClampBorder * 2);
				storedTextures[decal.texture] = newRect;

				repackAtlas = true;
			}
		}

	}

	// Update atlas texture if it is invalidated:
	if (repackAtlas)
	{
		rect_xywhf** out_rects = new rect_xywhf*[storedTextures.size()];
		int i = 0;
		for (auto& it : storedTextures)
		{
			out_rects[i] = &it.second;
			i++;
		}

		std::vector<bin> bins;
		if (pack(out_rects, (int)storedTextures.size(), 16384, bins))
		{
			assert(bins.size() == 1 && "The regions won't fit into the texture!");

			SAFE_DELETE(atlasTexture);

			TextureDesc desc;
			ZeroMemory(&desc, sizeof(desc));
			desc.Width = (UINT)bins[0].size.w;
			desc.Height = (UINT)bins[0].size.h;
			desc.MipLevels = 0;
			desc.ArraySize = 1;
			desc.Format = FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = USAGE_DEFAULT;
			desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags = 0;

			atlasTexture = new Texture2D;
			atlasTexture->RequestIndependentUnorderedAccessResourcesForMIPs(true);

			device->CreateTexture2D(&desc, nullptr, &atlasTexture);

			for (UINT mip = 0; mip < atlasTexture->GetDesc().MipLevels; ++mip)
			{
				for (auto& it : storedTextures)
				{
					if (mip < it.first->GetDesc().MipLevels)
					{
						//device->CopyTexture2D_Region(atlasTexture, mip, it.second.x >> mip, it.second.y >> mip, it.first, mip, threadID);

						// This is better because it implements format conversion so we can use multiple decal source texture formats in the atlas:
						CopyTexture2D(atlasTexture, mip, (it.second.x >> mip) + atlasClampBorder, (it.second.y >> mip) + atlasClampBorder, it.first, mip, threadID, BORDEREXPAND_CLAMP);
					}
				}
			}
		}
		else
		{
			wiBackLog::post("Decal atlas packing failed!");
		}

		SAFE_DELETE_ARRAY(out_rects);
	}

	// Assign atlas buckets to decals:
	for (size_t i = 0; i < scene.decals.GetCount(); ++i)
	{
		DecalComponent& decal = scene.decals[i];

		if (decal.texture != nullptr)
		{
			const TextureDesc& desc = atlasTexture->GetDesc();

			rect_xywhf rect = storedTextures[decal.texture];

			// eliminate border expansion:
			rect.x += atlasClampBorder;
			rect.y += atlasClampBorder;
			rect.w -= atlasClampBorder * 2;
			rect.h -= atlasClampBorder * 2;

			decal.atlasMulAdd = XMFLOAT4((float)rect.w / (float)desc.Width, (float)rect.h / (float)desc.Height, (float)rect.x / (float)desc.Width, (float)rect.y / (float)desc.Height);
		}
		else
		{
			decal.atlasMulAdd = XMFLOAT4(0, 0, 0, 0);
		}

	}

	if (atlasTexture != nullptr)
	{
		device->BindResource(PS, atlasTexture, TEXSLOT_DECALATLAS, threadID);
	}
}

void wiRenderer::UpdateWorldCB(GRAPHICSTHREAD threadID)
{
	const Scene& scene = GetScene();

	static WorldCB prevcb[GRAPHICSTHREAD_COUNT];

	WorldCB value;
	ZeroMemory(&value, sizeof(value));

	value.mScreenWidthHeight = XMFLOAT2((float)GetDevice()->GetScreenWidth(), (float)GetDevice()->GetScreenHeight());
	value.mScreenWidthHeight_Inverse = XMFLOAT2(1.0f / value.mScreenWidthHeight.x, 1.0f / value.mScreenWidthHeight.y);
	value.mInternalResolution = XMFLOAT2((float)GetInternalResolution().x, (float)GetInternalResolution().y);
	value.mInternalResolution_Inverse = XMFLOAT2(1.0f / value.mInternalResolution.x, 1.0f / value.mInternalResolution.y);
	value.mGamma = GetGamma();
	value.mAmbient = scene.ambient;
	value.mCloudiness = scene.cloudiness;
	value.mCloudScale = scene.cloudScale;
	value.mFog = XMFLOAT3(scene.fogStart, scene.fogEnd, scene.fogHeight);
	value.mHorizon = scene.horizon;
	value.mZenith = scene.zenith;
	value.mSpecularAA = SPECULARAA;
	value.mVoxelRadianceDataSize = voxelSceneData.voxelsize;
	value.mVoxelRadianceDataSize_Inverse = 1.0f / (float)value.mVoxelRadianceDataSize;
	value.mVoxelRadianceDataRes = GetVoxelRadianceEnabled() ? (UINT)voxelSceneData.res : 0;
	value.mVoxelRadianceDataRes_Inverse = 1.0f / (float)value.mVoxelRadianceDataRes;
	value.mVoxelRadianceDataMIPs = voxelSceneData.mips;
	value.mVoxelRadianceDataNumCones = max(min(voxelSceneData.numCones, 16), 1);
	value.mVoxelRadianceDataNumCones_Inverse = 1.0f / (float)value.mVoxelRadianceDataNumCones;
	value.mVoxelRadianceDataRayStepSize = voxelSceneData.rayStepSize;
	value.mVoxelRadianceReflectionsEnabled = voxelSceneData.reflectionsEnabled;
	value.mVoxelRadianceDataCenter = voxelSceneData.center;
	value.mAdvancedRefractions = GetAdvancedRefractionsEnabled() ? 1 : 0;
	value.mEntityCullingTileCount = GetEntityCullingTileCount();
	value.mTransparentShadowsEnabled = TRANSPARENTSHADOWSENABLED;
	value.mGlobalEnvProbeIndex = -1;
	value.mEnvProbeMipCount = 0;
	value.mEnvProbeMipCount_Inverse = 1.0f;
	if (scene.probes.GetCount() > 0)
	{
		value.mGlobalEnvProbeIndex = 0; // for now, the global envprobe will be the first probe in the array. Easy change later on if required...
	}
	if (textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY] != nullptr)
	{
		value.mEnvProbeMipCount = static_cast<Texture2D*>(textures[TEXTYPE_CUBEARRAY_ENVMAPARRAY])->GetDesc().MipLevels;
		value.mEnvProbeMipCount_Inverse = 1.0f / (float)value.mEnvProbeMipCount;
	}

	if (memcmp(&prevcb[threadID], &value, sizeof(WorldCB)) != 0) // prevent overcommit
	{
		prevcb[threadID] = value;
		GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_WORLD], &prevcb[threadID], threadID);
	}
}
void wiRenderer::UpdateFrameCB(GRAPHICSTHREAD threadID)
{
	const Scene& scene = GetScene();

	FrameCB cb;

	cb.mTime = renderTime;
	cb.mTimePrev = renderTime_Prev;
	cb.mDeltaTime = deltaTime;
	cb.mLightArrayOffset = entityArrayOffset_Lights;
	cb.mLightArrayCount = entityArrayCount_Lights;
	cb.mDecalArrayOffset = entityArrayOffset_Decals;
	cb.mDecalArrayCount = entityArrayCount_Decals;
	cb.mForceFieldArrayOffset = entityArrayOffset_ForceFields;
	cb.mForceFieldArrayCount = entityArrayCount_ForceFields;
	cb.mEnvProbeArrayOffset = entityArrayOffset_EnvProbes;
	cb.mEnvProbeArrayCount = entityArrayCount_EnvProbes;
	cb.mVoxelRadianceRetargetted = voxelSceneData.centerChangedThisFrame ? 1 : 0;
	cb.mWindRandomness = scene.windRandomness;
	cb.mWindWaveSize = scene.windWaveSize;
	cb.mWindDirection = scene.windDirection;
	cb.mFrameCount = (UINT)GetDevice()->GetFrameCount();
	cb.mSunEntityArrayIndex = -1;
	cb.mTemporalAASampleRotation = 0;
	if (GetTemporalAAEnabled())
	{
		UINT id = cb.mFrameCount % 4;
		UINT x = 0;
		UINT y = 0;
		switch (id)
		{
		case 1:
			x = 1;
			break;
		case 2:
			y = 1;
			break;
		case 3:
			x = 1;
			y = 1;
			break;
		default:
			break;
		}
		cb.mTemporalAASampleRotation = (x & 0x000000FF) | ((y & 0x000000FF) << 8);
	}
	cb.mTemporalAAJitter = temporalAAJitter;
	cb.mTemporalAAJitterPrev = temporalAAJitterPrev;

	auto camera = getCamera();
	auto prevCam = getPrevCamera();
	auto reflCam = getRefCamera();

	cb.mVP = XMMatrixTranspose(camera->GetViewProjection());
	cb.mView = XMMatrixTranspose(camera->GetView());
	cb.mProj = XMMatrixTranspose(camera->GetProjection());
	cb.mCamPos = camera->Eye;
	cb.mCamDistanceFromOrigin = XMVectorGetX(XMVector3Length(XMLoadFloat3(&cb.mCamPos)));
	cb.mPrevV = XMMatrixTranspose(prevCam->GetView());
	cb.mPrevP = XMMatrixTranspose(prevCam->GetProjection());
	cb.mPrevVP = XMMatrixTranspose(prevCam->GetViewProjection());
	cb.mPrevInvVP = XMMatrixTranspose(prevCam->GetInvViewProjection());
	cb.mReflVP = XMMatrixTranspose(reflCam->GetViewProjection());
	cb.mInvV = XMMatrixTranspose(camera->GetInvView());
	cb.mInvP = XMMatrixTranspose(camera->GetInvProjection());
	cb.mInvVP = XMMatrixTranspose(camera->GetInvViewProjection());
	cb.mAt = camera->At;
	cb.mUp = camera->Up;
	cb.mZNearP = camera->zNearP;
	cb.mZFarP = camera->zFarP;
	cb.mZNearP_Recip = 1.0f / max(0.0001f, cb.mZNearP);
	cb.mZFarP_Recip = 1.0f / max(0.0001f, cb.mZFarP);
	cb.mZRange = abs(cb.mZFarP - cb.mZNearP);
	cb.mZRange_Recip = 1.0f / max(0.0001f, cb.mZRange);
	cb.mFrustumPlanesWS[0] = camera->frustum.getLeftPlane();
	cb.mFrustumPlanesWS[1] = camera->frustum.getRightPlane();
	cb.mFrustumPlanesWS[2] = camera->frustum.getTopPlane();
	cb.mFrustumPlanesWS[3] = camera->frustum.getBottomPlane();
	cb.mFrustumPlanesWS[4] = camera->frustum.getNearPlane();
	cb.mFrustumPlanesWS[5] = camera->frustum.getFarPlane();

	cb.mWorldBoundsMin = scene.bounds.getMin();
	cb.mWorldBoundsMax = scene.bounds.getMax();
	cb.mWorldBoundsExtents.x = abs(cb.mWorldBoundsMax.x - cb.mWorldBoundsMin.x);
	cb.mWorldBoundsExtents.y = abs(cb.mWorldBoundsMax.y - cb.mWorldBoundsMin.y);
	cb.mWorldBoundsExtents.z = abs(cb.mWorldBoundsMax.z - cb.mWorldBoundsMin.z);
	cb.mWorldBoundsExtents_Inverse.x = 1.0f / cb.mWorldBoundsExtents.x;
	cb.mWorldBoundsExtents_Inverse.y = 1.0f / cb.mWorldBoundsExtents.y;
	cb.mWorldBoundsExtents_Inverse.z = 1.0f / cb.mWorldBoundsExtents.z;

	GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_FRAME], &cb, threadID);
}
void wiRenderer::UpdateCameraCB(CameraComponent* camera, GRAPHICSTHREAD threadID)
{
	CameraCB cb;

	cb.mVP = XMMatrixTranspose(camera->GetViewProjection());
	cb.mView = XMMatrixTranspose(camera->GetView());
	cb.mProj = XMMatrixTranspose(camera->GetProjection());
	cb.mCamPos = camera->Eye;

	GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_CAMERA], &cb, threadID);
}
wiRenderer::APICB apiCB[GRAPHICSTHREAD_COUNT];
void wiRenderer::SetClipPlane(const XMFLOAT4& clipPlane, GRAPHICSTHREAD threadID)
{
	apiCB[threadID].clipPlane = clipPlane;
	GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_API], &apiCB[threadID], threadID);
}
void wiRenderer::SetAlphaRef(float alphaRef, GRAPHICSTHREAD threadID)
{
	if (alphaRef != apiCB[threadID].alphaRef)
	{
		apiCB[threadID].alphaRef = alphaRef;
		GetDevice()->UpdateBuffer(constantBuffers[CBTYPE_API], &apiCB[threadID], threadID);
	}
}
void wiRenderer::BindGBufferTextures(Texture2D* slot0, Texture2D* slot1, Texture2D* slot2, Texture2D* slot3, Texture2D* slot4, GRAPHICSTHREAD threadID)
{
	GetDevice()->BindResource(PS, slot0, TEXSLOT_GBUFFER0, threadID);
	GetDevice()->BindResource(PS, slot1, TEXSLOT_GBUFFER1, threadID);
	GetDevice()->BindResource(PS, slot2, TEXSLOT_GBUFFER2, threadID);
	GetDevice()->BindResource(PS, slot3, TEXSLOT_GBUFFER3, threadID);
	GetDevice()->BindResource(PS, slot4, TEXSLOT_GBUFFER4, threadID);

	GetDevice()->BindResource(CS, slot0, TEXSLOT_GBUFFER0, threadID);
	GetDevice()->BindResource(CS, slot1, TEXSLOT_GBUFFER1, threadID);
	GetDevice()->BindResource(CS, slot2, TEXSLOT_GBUFFER2, threadID);
	GetDevice()->BindResource(CS, slot3, TEXSLOT_GBUFFER3, threadID);
	GetDevice()->BindResource(CS, slot4, TEXSLOT_GBUFFER4, threadID);
}
void wiRenderer::BindDepthTextures(Texture2D* depth, Texture2D* linearDepth, GRAPHICSTHREAD threadID)
{
	GetDevice()->BindResource(PS, depth, TEXSLOT_DEPTH, threadID);
	GetDevice()->BindResource(VS, depth, TEXSLOT_DEPTH, threadID);
	GetDevice()->BindResource(GS, depth, TEXSLOT_DEPTH, threadID);
	GetDevice()->BindResource(CS, depth, TEXSLOT_DEPTH, threadID);

	GetDevice()->BindResource(PS, linearDepth, TEXSLOT_LINEARDEPTH, threadID);
	GetDevice()->BindResource(VS, linearDepth, TEXSLOT_LINEARDEPTH, threadID);
	GetDevice()->BindResource(GS, linearDepth, TEXSLOT_LINEARDEPTH, threadID);
	GetDevice()->BindResource(CS, linearDepth, TEXSLOT_LINEARDEPTH, threadID);
}


Texture2D* wiRenderer::GetLuminance(Texture2D* sourceImage, GRAPHICSTHREAD threadID)
{
	GraphicsDevice* device = wiRenderer::GetDevice();

	static Texture2D* luminance_map = nullptr;
	static std::vector<Texture2D*> luminance_avg(0);
	if (luminance_map == nullptr)
	{
		SAFE_DELETE(luminance_map);
		for (auto& x : luminance_avg)
		{
			SAFE_DELETE(x);
		}
		luminance_avg.clear();

		// lower power of two
		//UINT minRes = wiMath::GetNextPowerOfTwo(min(device->GetScreenWidth(), device->GetScreenHeight())) / 2;

		TextureDesc desc;
		ZeroMemory(&desc, sizeof(desc));
		desc.Width = 256;
		desc.Height = desc.Width;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = FORMAT_R32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Usage = USAGE_DEFAULT;
		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;

		device->CreateTexture2D(&desc, nullptr, &luminance_map);

		while (desc.Width > 1)
		{
			desc.Width = max(desc.Width / 16, 1);
			desc.Height = desc.Width;

			Texture2D* tex = nullptr;
			device->CreateTexture2D(&desc, nullptr, &tex);

			luminance_avg.push_back(tex);
		}
	}
	if (luminance_map != nullptr)
	{
		// Pass 1 : Create luminance map from scene tex
		TextureDesc luminance_map_desc = luminance_map->GetDesc();
		device->BindComputePSO(CPSO[CSTYPE_LUMINANCE_PASS1], threadID);
		device->BindResource(CS, sourceImage, TEXSLOT_ONDEMAND0, threadID);
		device->BindUAV(CS, luminance_map, 0, threadID);
		device->Dispatch(luminance_map_desc.Width/16, luminance_map_desc.Height/16, 1, threadID);

		// Pass 2 : Reduce for average luminance until we got an 1x1 texture
		TextureDesc luminance_avg_desc;
		for (size_t i = 0; i < luminance_avg.size(); ++i)
		{
			luminance_avg_desc = luminance_avg[i]->GetDesc();
			device->BindComputePSO(CPSO[CSTYPE_LUMINANCE_PASS2], threadID);
			device->BindUAV(CS, luminance_avg[i], 0, threadID);
			if (i > 0)
			{
				device->BindResource(CS, luminance_avg[i-1], TEXSLOT_ONDEMAND0, threadID);
			}
			else
			{
				device->BindResource(CS, luminance_map, TEXSLOT_ONDEMAND0, threadID);
			}
			device->Dispatch(luminance_avg_desc.Width, luminance_avg_desc.Height, 1, threadID);
		}


		device->UnbindUAVs(0, 1, threadID);

		return luminance_avg.back();
	}

	return nullptr;
}

const XMFLOAT4& wiRenderer::GetWaterPlane()
{
	return waterPlane;
}


RAY wiRenderer::getPickRay(long cursorX, long cursorY) {
	const CameraComponent& camera = *getCamera();
	XMMATRIX V = camera.GetView();
	XMMATRIX P = camera.GetRealProjection();
	XMMATRIX W = XMMatrixIdentity();
	XMVECTOR& lineStart = XMVector3Unproject(XMVectorSet((float)cursorX, (float)cursorY, 0, 1), 0, 0, camera.width, camera.height, 0.0f, 1.0f, P, V, W);
	XMVECTOR& lineEnd = XMVector3Unproject(XMVectorSet((float)cursorX, (float)cursorY, 1, 1), 0, 0, camera.width, camera.height, 0.0f, 1.0f, P, V, W);
	XMVECTOR& rayDirection = XMVector3Normalize(XMVectorSubtract(lineEnd, lineStart));
	return RAY(lineStart, rayDirection);
}

wiRenderer::RayIntersectWorldResult wiRenderer::RayIntersectWorld(const RAY& ray, UINT renderTypeMask, uint32_t layerMask)
{
	Scene& scene = GetScene();

	RayIntersectWorldResult result;

	if (scene.objects.GetCount() > 0)
	{
		const XMVECTOR rayOrigin = XMLoadFloat3(&ray.origin);
		const XMVECTOR rayDirection = XMVector3Normalize(XMLoadFloat3(&ray.direction));

		for (size_t i = 0; i < scene.aabb_objects.GetCount(); ++i)
		{
			const AABB& aabb = scene.aabb_objects[i];
			if (!ray.intersects(aabb))
			{
				continue;
			}

			const ObjectComponent& object = scene.objects[i];
			if (object.meshID == INVALID_ENTITY)
			{
				continue;
			}
			if (!(renderTypeMask & object.GetRenderTypes()))
			{
				continue;
			}

			Entity entity = scene.aabb_objects.GetEntity(i);
			const LayerComponent& layer = *scene.layers.GetComponent(entity);

			if (layer.GetLayerMask() & layerMask)
			{
				const MeshComponent& mesh = *scene.meshes.GetComponent(object.meshID);

				const TransformComponent& transform = *scene.transforms.GetComponent(entity);

				const XMMATRIX objectMat = XMLoadFloat4x4(&transform.world);
				const XMMATRIX objectMat_Inverse = XMMatrixInverse(nullptr, objectMat);

				const XMVECTOR rayOrigin_local = XMVector3Transform(rayOrigin, objectMat_Inverse);
				const XMVECTOR rayDirection_local = XMVector3Normalize(XMVector3TransformNormal(rayDirection, objectMat_Inverse));

				int subsetCounter = 0;
				for (auto& subset : mesh.subsets)
				{
					for (size_t i = 0; i < subset.indexCount; i += 3)
					{
						uint32_t i0 = mesh.indices[subset.indexOffset + i + 0];
						uint32_t i1 = mesh.indices[subset.indexOffset + i + 1];
						uint32_t i2 = mesh.indices[subset.indexOffset + i + 2];

						XMVECTOR p0 = XMLoadFloat3(&mesh.vertex_positions[i0]);
						XMVECTOR p1 = XMLoadFloat3(&mesh.vertex_positions[i1]);
						XMVECTOR p2 = XMLoadFloat3(&mesh.vertex_positions[i2]);

						if (mesh.IsSkinned())
						{
							const ArmatureComponent& armature = *scene.armatures.GetComponent(mesh.armatureID);

							const XMUINT4& ind0 = mesh.vertex_boneindices[i0];
							const XMUINT4& ind1 = mesh.vertex_boneindices[i1];
							const XMUINT4& ind2 = mesh.vertex_boneindices[i2];

							const XMFLOAT4& wei0 = mesh.vertex_boneweights[i0];
							const XMFLOAT4& wei1 = mesh.vertex_boneweights[i1];
							const XMFLOAT4& wei2 = mesh.vertex_boneweights[i2];

							XMMATRIX sump;

							sump  = XMLoadFloat4x4(&armature.skinningMatrices[ind0.x]) * wei0.x;
							sump += XMLoadFloat4x4(&armature.skinningMatrices[ind0.y]) * wei0.y;
							sump += XMLoadFloat4x4(&armature.skinningMatrices[ind0.z]) * wei0.z;
							sump += XMLoadFloat4x4(&armature.skinningMatrices[ind0.w]) * wei0.w;

							p0 = XMVector3Transform(p0, sump);

							sump  = XMLoadFloat4x4(&armature.skinningMatrices[ind1.x]) * wei1.x;
							sump += XMLoadFloat4x4(&armature.skinningMatrices[ind1.y]) * wei1.y;
							sump += XMLoadFloat4x4(&armature.skinningMatrices[ind1.z]) * wei1.z;
							sump += XMLoadFloat4x4(&armature.skinningMatrices[ind1.w]) * wei1.w;

							p1 = XMVector3Transform(p1, sump);

							sump  = XMLoadFloat4x4(&armature.skinningMatrices[ind2.x]) * wei2.x;
							sump += XMLoadFloat4x4(&armature.skinningMatrices[ind2.y]) * wei2.y;
							sump += XMLoadFloat4x4(&armature.skinningMatrices[ind2.z]) * wei2.z;
							sump += XMLoadFloat4x4(&armature.skinningMatrices[ind2.w]) * wei2.w;

							p2 = XMVector3Transform(p2, sump);
						}

						float distance;
						if (TriangleTests::Intersects(rayOrigin_local, rayDirection_local, p0, p1, p2, distance))
						{
							XMVECTOR pos = XMVector3Transform(XMVectorAdd(rayOrigin_local, rayDirection_local*distance), objectMat);
							distance = wiMath::Distance(pos, rayOrigin);

							if (distance < result.distance)
							{
								XMVECTOR nor = XMVector3Normalize(XMVector3TransformNormal(XMVector3Normalize(XMVector3Cross(XMVectorSubtract(p2, p1), XMVectorSubtract(p1, p0))), objectMat));

								result.entity = entity;
								XMStoreFloat3(&result.position, pos);
								XMStoreFloat3(&result.normal, nor);
								result.distance = distance;
								result.subsetIndex = subsetCounter;
							}
						}
					}
					subsetCounter++;
				}

			}

		}
	}

	return result;
}

Entity wiRenderer::LoadModel(const std::string& fileName, const XMMATRIX& transform)
{
	//Model* model = nullptr;

	//wiArchive archive(fileName, true);
	//if (archive.IsOpen())
	//{
	//	model = new Model;
	//	model->Serialize(archive);
	//	model->transform(transform);

	//	AddModel(model);
	//}
	//else
	//{
	//	wiHelper::messageBox("Could not open archive!", "Error!");
	//}

	//LoadWorldInfo(fileName);

	//return model;

	return INVALID_ENTITY;
}
void wiRenderer::LoadWorldInfo(const std::string& fileName)
{
	Scene& scene = GetScene();

	string extension = wiHelper::GetExtensionFromFileName(fileName);

	string realName;
	if (!extension.compare("wiw"))
	{
		realName = fileName;
	}
	else if (extension.empty())
	{
		realName = fileName + ".wiw";
	}
	else
	{
		realName = fileName;
		wiHelper::RemoveExtensionFromFileName(realName);
		realName += ".wiw";
	}

	ifstream file(realName);
	if (file)
	{
		while (!file.eof())
		{
			string read = "";
			file >> read;
			switch (read[0])
			{
			case 'h':
				file >> scene.horizon.x >> scene.horizon.y >> scene.horizon.z;
				// coming from blender, de-apply gamma correction:
				scene.horizon.x = powf(scene.horizon.x, 1.0f / 2.2f);
				scene.horizon.y = powf(scene.horizon.y, 1.0f / 2.2f);
				scene.horizon.z = powf(scene.horizon.z, 1.0f / 2.2f);
				break;
			case 'z':
				file >> scene.zenith.x >> scene.zenith.y >> scene.zenith.z;
				// coming from blender, de-apply gamma correction:
				scene.zenith.x = powf(scene.zenith.x, 1.0f / 2.2f);
				scene.zenith.y = powf(scene.zenith.y, 1.0f / 2.2f);
				scene.zenith.z = powf(scene.zenith.z, 1.0f / 2.2f);
				break;
			case 'a':
				file >> scene.ambient.x >> scene.ambient.y >> scene.ambient.z;
				// coming from blender, de-apply gamma correction:
				scene.zenith.x = powf(scene.zenith.x, 1.0f / 2.2f);
				scene.zenith.y = powf(scene.zenith.y, 1.0f / 2.2f);
				scene.zenith.z = powf(scene.zenith.z, 1.0f / 2.2f);
				break;
			case 'W':
			{
				XMFLOAT4 r;
				float s;
				file >> r.x >> r.y >> r.z >> r.w >> s;
				XMStoreFloat3(&scene.windDirection, XMVector3Transform(XMVectorSet(0, s, 0, 0), XMMatrixRotationQuaternion(XMLoadFloat4(&r))));
			}
			break;
			case 'm':
			{
				file >> scene.fogStart >> scene.fogEnd >> scene.fogHeight;
			}
			break;
			default:break;
			}
		}
	}
	file.close();
}
Scene& wiRenderer::GetScene()
{
	if (scene == nullptr)
	{
		scene = new Scene;

		scene->transforms.Create(cameraID);
		scene->cameras.Create(cameraID).CreatePerspective((float)wiRenderer::GetInternalResolution().x, (float)wiRenderer::GetInternalResolution().y, 0.1f, 800);
	}
	return *scene;
}

void wiRenderer::SynchronizeWithPhysicsEngine(float dt)
{
	//if (physicsEngine && GetGameSpeed())
	//{
	//	physicsEngine->addWind(GetScene().worldInfo.windDirection);

	//	// Update physics world data
	//	for (Model* model : GetScene().models)
	//	{
	//		for (Object* object : model->objects) 
	//		{
	//			Mesh* mesh = object->mesh;
	//			int pI = object->physicsObjectID;

	//			if (pI < 0 && (object->rigidBody || mesh->softBody))
	//			{
	//				// Register the objects with physics attributes that doesn't exist in the simulation
	//				physicsEngine->registerObject(object);
	//			}

	//			if (pI >= 0) 
	//			{
	//				if (mesh->softBody) 
	//				{
	//					int gvg = mesh->goalVG;
	//					if (gvg >= 0)
	//					{
	//						XMMATRIX worldMat = mesh->hasArmature() ? XMMatrixIdentity() : XMLoadFloat4x4(&object->world);
	//						int j = 0;
	//						for (std::map<int, float>::iterator it = mesh->vertexGroups[gvg].vertices.begin(); it != mesh->vertexGroups[gvg].vertices.end(); ++it)
	//						{
	//							int vi = (*it).first;
	//							MeshComponent::Vertex_FULL tvert = mesh->TransformVertex(vi, worldMat);
	//							mesh->goalPositions[j] = XMFLOAT3(tvert.pos.x, tvert.pos.y, tvert.pos.z);
	//							mesh->goalNormals[j] = XMFLOAT3(tvert.nor.x, tvert.nor.y, tvert.nor.z);
	//							++j;
	//						}
	//					}
	//					physicsEngine->connectSoftBodyToVertices(
	//						object->mesh, pI
	//						);
	//				}
	//				if (object->kinematic && object->rigidBody)
	//				{
	//					physicsEngine->transformBody(object->rotation, object->translation, pI);
	//				}
	//			}
	//		}
	//	}

	//	// Run physics simulation
	//	physicsEngine->Update(dt);

	//	// Retrieve physics simulation data
	//	for (Model* model : GetScene().models)
	//	{
	//		for (Object* object : model->objects) {
	//			int pI = object->physicsObjectID;
	//			if (pI >= 0 && !object->kinematic && (object->rigidBody || object->mesh->softBody)) {
	//				PHYSICS::PhysicsTransform* transform(physicsEngine->getObject(pI));
	//				object->translation_rest = transform->position;
	//				object->rotation_rest = transform->rotation;

	//				if (object->mesh->softBody) {
	//					object->scale_rest = XMFLOAT3(1, 1, 1);
	//					physicsEngine->connectVerticesToSoftBody(
	//						object->mesh, pI
	//						);
	//				}
	//			}
	//		}
	//	}


	//	physicsEngine->NextRunWorld();
	//}
}

void wiRenderer::CreateImpostor(Entity entity, GRAPHICSTHREAD threadID)
{
	Scene& scene = GetScene();

	MeshComponent& mesh = *scene.meshes.GetComponent(entity);

	static const int res = 256;

	const AABB& bbox = mesh.aabb;
	const XMFLOAT3 extents = bbox.getHalfWidth();
	if (!mesh.impostorTarget.IsInitialized())
	{
		mesh.impostorTarget.Initialize(res * 6, res, true, RTFormat_impostor_albedo, 0);
		mesh.impostorTarget.Add(RTFormat_impostor_normal);		// normal, roughness
		mesh.impostorTarget.Add(RTFormat_impostor_surface);		// surface properties
	}


	CameraComponent camera = *getCamera();
	TransformComponent camera_transform = *scene.transforms.GetComponent(getCameraID());

	BindPersistentState(threadID);

	const XMFLOAT4X4 __identity = XMFLOAT4X4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
	struct InstBuf
	{
		Instance instance;
		InstancePrev instancePrev;
	};
	UINT instancesOffset; 
	volatile InstBuf* buff = (volatile InstBuf*)GetDevice()->AllocateFromRingBuffer(dynamicVertexBufferPool, sizeof(InstBuf), instancesOffset, threadID);
	buff->instance.Create(__identity);
	buff->instancePrev.Create(__identity);
	GetDevice()->InvalidateBufferAccess(dynamicVertexBufferPool, threadID);

	GPUBuffer* vbs[] = {
		mesh.IsSkinned() ? mesh.streamoutBuffer_POS.get() : mesh.vertexBuffer_POS.get(),
		mesh.vertexBuffer_TEX.get(),
		mesh.IsSkinned() ? mesh.streamoutBuffer_PRE.get() : mesh.vertexBuffer_POS.get(),
		dynamicVertexBufferPool
	};
	UINT strides[] = {
		sizeof(MeshComponent::Vertex_POS),
		sizeof(MeshComponent::Vertex_TEX),
		sizeof(MeshComponent::Vertex_POS),
		sizeof(InstBuf)
	};
	UINT offsets[] = {
		0,
		0,
		0,
		instancesOffset
	};
	GetDevice()->BindVertexBuffers(vbs, 0, ARRAYSIZE(vbs), strides, offsets, threadID);

	GetDevice()->BindIndexBuffer(mesh.indexBuffer.get(), mesh.GetIndexFormat(), 0, threadID);

	GetDevice()->BindGraphicsPSO(PSO_captureimpostor, threadID);

	ViewPort savedViewPort = mesh.impostorTarget.viewPort;
	mesh.impostorTarget.Activate(threadID, 0, 0, 0, 0);
	for (size_t i = 0; i < 6; ++i)
	{
		mesh.impostorTarget.viewPort.Height = (float)res;
		mesh.impostorTarget.viewPort.Width = (float)res;
		mesh.impostorTarget.viewPort.TopLeftX = (float)(i*res);
		mesh.impostorTarget.viewPort.TopLeftY = 0.f;
		mesh.impostorTarget.Set(threadID);

		camera_transform.ClearTransform();
		camera_transform.Translate(bbox.getCenter());
		switch (i)
		{
		case 0:
		{
			// front capture
			XMMATRIX ortho = XMMatrixOrthographicOffCenterLH(-extents.x, extents.x, -extents.y, extents.y, -extents.z, extents.z);
			XMStoreFloat4x4(&camera.Projection, ortho);
		}
		break;
		case 1:
		{
			// right capture
			XMMATRIX ortho = XMMatrixOrthographicOffCenterLH(-extents.z, extents.z, -extents.y, extents.y, -extents.x, extents.x);
			XMStoreFloat4x4(&camera.Projection, ortho);
			camera_transform.RotateRollPitchYaw(XMFLOAT3(0, -XM_PIDIV2, 0));
		}
		break;
		case 2:
		{
			// back capture
			XMMATRIX ortho = XMMatrixOrthographicOffCenterLH(-extents.x, extents.x, -extents.y, extents.y, -extents.z, extents.z);
			XMStoreFloat4x4(&camera.Projection, ortho);
			camera_transform.RotateRollPitchYaw(XMFLOAT3(0, -XM_PI, 0));
		}
		break;
		case 3:
		{
			// left capture
			XMMATRIX ortho = XMMatrixOrthographicOffCenterLH(-extents.z, extents.z, -extents.y, extents.y, -extents.x, extents.x);
			XMStoreFloat4x4(&camera.Projection, ortho);
			camera_transform.RotateRollPitchYaw(XMFLOAT3(0, XM_PIDIV2, 0));
		}
		break;
		case 4:
		{
			// bottom capture
			XMMATRIX ortho = XMMatrixOrthographicOffCenterLH(-extents.x, extents.x, -extents.z, extents.z, -extents.y, extents.y);
			XMStoreFloat4x4(&camera.Projection, ortho);
			camera_transform.RotateRollPitchYaw(XMFLOAT3(-XM_PIDIV2, 0, 0));
		}
		break;
		case 5:
		{
			// top capture
			XMMATRIX ortho = XMMatrixOrthographicOffCenterLH(-extents.x, extents.x, -extents.z, extents.z, -extents.y, extents.y);
			XMStoreFloat4x4(&camera.Projection, ortho);
			camera_transform.RotateRollPitchYaw(XMFLOAT3(XM_PIDIV2, 0, 0));
		}
		break;
		default:
			break;
		}
		camera_transform.UpdateTransform();
		camera.UpdateCamera(&camera_transform);
		camera.UpdateProjection();
		UpdateCameraCB(&camera, threadID);

		for (MeshComponent::MeshSubset& subset : mesh.subsets)
		{
			if (subset.indexCount == 0)
			{
				continue;
			}
			MaterialComponent& material = *GetScene().materials.GetComponent(subset.materialID);

			GetDevice()->BindConstantBuffer(PS, material.constantBuffer.get(), CB_GETBINDSLOT(MaterialCB), threadID);

			GetDevice()->BindResource(PS, material.GetBaseColorMap(), TEXSLOT_ONDEMAND0, threadID);
			GetDevice()->BindResource(PS, material.GetNormalMap(), TEXSLOT_ONDEMAND1, threadID);
			GetDevice()->BindResource(PS, material.GetSurfaceMap(), TEXSLOT_ONDEMAND2, threadID);

			GetDevice()->DrawIndexedInstanced((int)subset.indexCount, 1, subset.indexOffset, 0, 0, threadID);

		}

	}
	wiRenderer::GenerateMipChain(mesh.impostorTarget.GetTexture(), wiRenderer::MIPGENFILTER_LINEAR, threadID);

	mesh.impostorTarget.viewPort = savedViewPort;
	UpdateCameraCB(getCamera(), threadID);
}

void wiRenderer::AddRenderableBox(const XMFLOAT4X4& boxMatrix, const XMFLOAT4& color)
{
	renderableBoxes.push_back(pair<XMFLOAT4X4,XMFLOAT4>(boxMatrix,color));
}
void wiRenderer::AddRenderableLine(const RenderableLine& line)
{
	renderableLines.push_back(line);
}

void wiRenderer::AddDeferredMIPGen(Texture2D* tex)
{
	deferredMIPGenLock.lock();
	deferredMIPGens.insert(tex);
	deferredMIPGenLock.unlock();
}


void wiRenderer::SetOcclusionCullingEnabled(bool value)
{
	static bool initialized = false;

	if (!initialized && value == true)
	{
		initialized = true;

		GPUQueryDesc desc;
		desc.Type = GPU_QUERY_TYPE_OCCLUSION_PREDICATE;
		desc.MiscFlags = 0;
		desc.async_latency = 1;

		for (int i = 0; i < ARRAYSIZE(occlusionQueries); ++i)
		{
			wiRenderer::GetDevice()->CreateQuery(&desc, &occlusionQueries[i]);
			occlusionQueries[i].result_passed = TRUE;
		}
	}

	occlusionCulling = value;
}

bool wiRenderer::GetAdvancedRefractionsEnabled()
{
	return advancedRefractions && GetDevice()->CheckCapability(GraphicsDevice::GRAPHICSDEVICE_CAPABILITY_UNORDEREDACCESSTEXTURE_LOAD_FORMAT_EXT);
}

void wiRenderer::SetOceanEnabled(bool enabled, const wiOceanParameter& params)
{
	SAFE_DELETE(ocean);

	if (enabled)
	{
		ocean = new wiOcean(params);
	}
}


CameraComponent* wiRenderer::getCamera()
{
	static CameraComponent camera;
	return &camera;
}
CameraComponent* wiRenderer::getPrevCamera()
{
	static CameraComponent camera;
	return &camera;
}
CameraComponent* wiRenderer::getRefCamera()
{
	static CameraComponent camera;
	return &camera;
}
