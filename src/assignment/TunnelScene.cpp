#include "TunnelScene.h"
#include "gui/Image.hpp"
#include "surf.h"
#include "Globals.h"
#include "TessellationTestScene.h"
#include "Util.h"
#include "SyncVars.h"
#include <fstream>
#include <unordered_map>
#include <algorithm>
#include "curve.h"

namespace FW {

	TunnelScene::TunnelScene(GLContext * ctx, int width, int height, FBO * lastPass, CameraControls * camPtr, TessellationTestScene * tessScene)

		: mWidth(width),
		  mHeight(height),
		  mLastFBO(lastPass),
		  mCamPtr(camPtr),
		  mTessScene(tessScene)

	{
		GLuint depthTex = TEXTURE_POOL->request(TextureDescriptor(GL_DEPTH_COMPONENT32F, width, height, GL_DEPTH_COMPONENT, GL_FLOAT))->m_texture;
		GLuint colorTex = TEXTURE_POOL->request(TextureDescriptor(GL_RGBA32F, width, height, GL_RGBA, GL_FLOAT))->m_texture;
		GLuint normalTex = TEXTURE_POOL->request(TextureDescriptor(GL_RGBA32F, width, height, GL_RGBA, GL_FLOAT))->m_texture;
		GLuint velocityTex = TEXTURE_POOL->request(TextureDescriptor(GL_RG16F, width, height, GL_RG, GL_FLOAT))->m_texture;
		GLuint motionBlurTex = TEXTURE_POOL->request(TextureDescriptor(GL_RGBA32F, width, height, GL_RGBA, GL_FLOAT))->m_texture;

		mGBuffer.reset(new FBO(depthTex));
		mGBuffer->attachTexture(0, colorTex);
		mGBuffer->attachTexture(1, normalTex);
		//mGBuffer->attachTexture(2, velocityTex);

		loadShaders(ctx);
		setupGLBuffers();
		generateTunnel();

		Image * tunnelDiffuseImg = FW::importImage("assets/tunnel/synthetic_metal_04_diffuse.png");
		mTunnelTexture = tunnelDiffuseImg->createGLTexture();

		Image * tunnelNormalImg = FW::importImage("assets/tunnel/synthetic_metal_04_normal.png");
		mTunnelNormalTexture = tunnelNormalImg->createGLTexture();

		Image * tunnelSpecularImg = FW::importImage("assets/tunnel/synthetic_metal_04_specular.png");
		mTunnelSpecularTexture = tunnelSpecularImg->createGLTexture();

		Image * bokehImage = FW::importImage("assets/bokeh.png");
		mBokehTexture = bokehImage->createGLTexture();
		delete bokehImage;

		Image * bokehImage2 = FW::importImage("assets/bokeh2.png");
		mBokehTextureStrip = bokehImage2->createGLTexture();
		delete bokehImage2;

		generateParticles();
		generateRibbon();
		loadCamPaths();

		mGodrayFBO.reset(new FBO(depthTex));
		mGodrayFBO->attachTexture(0, colorTex);
		GLuint godrayColorTex = TEXTURE_POOL->request(TextureDescriptor(GL_RGBA32F, width, height, GL_RGBA, GL_FLOAT))->m_texture;
		mGodrayBlurFBO.reset(new FBO(depthTex));
		mGodrayBlurFBO->attachTexture(0, godrayColorTex);

		mGodrayBlurTex = TEXTURE_POOL->request(TextureDescriptor(GL_RGBA32F, width, height, GL_RGBA, GL_FLOAT))->m_texture;

		mGaussiaFilter.reset(new GaussianFilter(ctx, Vec2i(width, height)));

	}

	void TunnelScene::generateRibbon()
	{



		std::vector<Vec3f> controlPoints = {
			Vec3f(1000,60000, 1000),
			Vec3f(1000,30000, 1000),
			Vec3f(1000,15000, 1000),
			Vec3f(1000,1000, 1000),
		};

		
		Curve meteorCurve = evalCatmullRomspline(controlPoints, 500, false, 0.0, 0.0);

		Curve starCurve = mTessScene->starCurve;
		for (size_t i = 0; i < starCurve.size(); ++i)
		{
			starCurve[i].V *= 3.5f;
		}

		Surface surf = makeGenCyl(starCurve, meteorCurve);

		mMeteorNumIndices = surf.VF.size();

		std::vector<MissileMeshVertex> vertexData(surf.VV.size());

		for (size_t k = 0; k < vertexData.size(); ++k)
		{
			vertexData[k] = MissileMeshVertex(surf.VV[k], surf.VN[k], surf.VT[k].x, surf.VT[k].y);
		}

		glGenVertexArrays(1, &mMeteorVAO);
		glGenBuffers(1, &mMeteorIBO);
		glGenBuffers(1, &mMeteorVBO);

		glBindVertexArray(mMeteorVAO);

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mMeteorIBO);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, surf.VF.size() * sizeof(Vec3i), surf.VF.data(), GL_STATIC_DRAW);

		glBindBuffer(GL_ARRAY_BUFFER, mMeteorVBO);
		glBufferData(GL_ARRAY_BUFFER, vertexData.size() * sizeof(MissileMeshVertex), vertexData.data(), GL_STATIC_DRAW);

		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(MissileMeshVertex), NULL);

		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(MissileMeshVertex), (char*)NULL + sizeof(Vec4f));

		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glBindVertexArray(0);
	}

	static struct CityParticleComparator {

		bool operator()(const ParticleInfo &a, const ParticleInfo &b)
		{
			return a.position.y < b.position.y;
		}

	};

	void TunnelScene::generateParticles() {

		//Mesh<VertexPNTC> * cityMesh = (Mesh<VertexPNTC>*)importMesh("assets/city/city_tesselated.obj");
		Mesh<VertexPNTC> * cityMesh = (Mesh<VertexPNTC>*)importMesh("assets/city/city_lightmapped.obj");

		std::vector<ParticleMaterial> materials(cityMesh->numSubmeshes());

		// texture handle, location
		std::unordered_map<GLuint, int> textureMap;
		std::unordered_map<GLuint, int>::iterator textureMapIterator;

		int numTriangles = cityMesh->numTriangles();
		int particlesPerTriangle = 19;
		int cParticle = 0;

		mNumCityParticles = particlesPerTriangle * numTriangles;

		Random rnd;

		std::vector<ParticleInfo> particleData(mNumCityParticles);
		for (int i = 0; i < cityMesh->numSubmeshes(); ++i)
		{
			// copy material
			const auto & cMaterial = cityMesh->material(i);;

			int useDiffuseTexId = -1;
			const auto & diffuseTex = cMaterial.textures[MeshBase::TextureType_Diffuse];
			if (diffuseTex.exists())
			{
				GLuint textureHandle = diffuseTex.getGLTexture();
				textureMapIterator = textureMap.find(textureHandle);

				if (textureMapIterator == textureMap.end())
				{
					GLuint64 cTexHandle = glGetTextureHandleARB(textureHandle);
					useDiffuseTexId = mTextureHandles.size();
					mTextureHandles.push_back(cTexHandle);
					textureMap[cTexHandle] = useDiffuseTexId;
				}
				else {
					useDiffuseTexId = textureMapIterator->second;
				}
			}

			materials[i] = ParticleMaterial(cMaterial.diffuse.getXYZ(), useDiffuseTexId, cMaterial.specular, -1);
		}

		// Spawn particles until we have hit the limit
		while (cParticle < mNumCityParticles) {

			for (int i = 0; i < cityMesh->numSubmeshes(); ++i)
			{

				const Array<Vec3i>& idx = cityMesh->indices(i);
				for (int j = 0; j < idx.getSize(); ++j)
				{

					const VertexPNTC &v0 = cityMesh->vertex(idx[j][0]),
						&v1 = cityMesh->vertex(idx[j][1]),
						&v2 = cityMesh->vertex(idx[j][2]);

					const Vec3f p0 = v1.p - v0.p;
					const Vec3f p1 = v2.p - v0.p;
					const float area = p0.cross(p1).length() / 2.0f;

					int count = (int)floor(area);

					for (int k = 0; k < count; ++k) {
						float u1 = rnd.getF32(0.0f, 1.0f);
						float u2 = rnd.getF32(0.0f, 1.0f);
						float su1 = sqrtf(u1);
						float u = 1.0f - su1;
						float v = su1 * u2;
						Vec3f point = barycentricInterpolation(u, v, v0.p, v1.p, v2.p);
						Vec3f rndVec = rnd.getVec3f(-1.0f, 1.0f).normalized();
						Vec3f normal = barycentricInterpolation(u, v, v0.n, v1.n, v2.n).normalized();
						Vec2f trigUV = barycentricInterpolation(u, v, v0.t, v1.t, v2.t);
						particleData[cParticle] = ParticleInfo(Vec4f(point, trigUV.x), Vec4f(normal, trigUV.y), i);
						++cParticle;

						if (cParticle >= mNumCityParticles) {
							goto generationDone;
						}
					}
				}
			}
		}

		// we jump here once enough particles have been spawned
		generationDone:

		std::sort(particleData.begin(), particleData.end(), CityParticleComparator());

		glGenBuffers(1, &mParticleMaterialSSBO);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, mParticleMaterialSSBO);

		glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(ParticleMaterial) * materials.size(), materials.data(), GL_STATIC_DRAW);

		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);



		glGenVertexArrays(1, &mCityVAO);
		glBindVertexArray(mCityVAO);

		glGenBuffers(1, &mCityVBO);
		glBindBuffer(GL_ARRAY_BUFFER, mCityVBO);

		glBufferData(GL_ARRAY_BUFFER, sizeof(ParticleInfo) * mNumCityParticles, particleData.data(), GL_STATIC_DRAW);

		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(ParticleInfo), (void*)NULL);

		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(ParticleInfo), (void*)(NULL + sizeof(Vec4f)));

		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(ParticleInfo), (void*)(NULL + 2 * sizeof(Vec4f)));

		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glBindVertexArray(0);


		glGenBuffers(1, &mCityVBOCopy);
		glBindBuffer(GL_ARRAY_BUFFER, mCityVBOCopy);
		glBufferData(GL_ARRAY_BUFFER, sizeof(ParticleInfo) * mNumCityParticles, particleData.data(), GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);

	}

	void TunnelScene::lightPass()
	{

	}

	void TunnelScene::render(Window & wnd, const CameraControls & camera) {
		GLContext *gl = wnd.getGL();

		explodeCity(gl);

		mCamPtr->setFar(400000);

		mCamPtr->setPosition(getCameraPosition());
		mCamPtr->setForward(getCameraForward());

		Mat4f camToClip = camera.getWorldToClip();
		Mat4f camToCamera = camera.getWorldToCamera();
		Vec3f cameraDir = camera.getForward();
		Vec3f cameraPos = camera.getPosition();

		Vec3f lightColor = Vec3f(0.5, 0.2, 0.2);
		Vec3f fogColor = Vec3f(0.0005, 0.0005f, 0.003f);

		Mat4f toLight, toLightClip;
		mTessScene->getLightMatrix(cameraPos, toLight, toLightClip);

		Vec3f lightPos = (toLight.inverted() * Vec4f(0, 0, 0, 1)).getXYZ();
		Vec3f lightDir = ((toLight * Vec4f(0, 0, 0, 1)).getXYZ() - lightPos).normalized();

		mGBuffer->bind();

		glClearColor(fogColor.x, fogColor.y, fogColor.z, 1);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		glDisable(GL_DEPTH_TEST);
		mBackgroundRenderProgram->use();
		

		gl->setUniform(mBackgroundRenderProgram->getUniformLoc("invClip"), camToClip.inverted());

		glBindVertexArray(mTessScene->m_quadVAO);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glBindVertexArray(0);
		glEnable(GL_DEPTH_TEST);



		renderCity(gl, camToClip, lightPos, lightDir, lightColor, fogColor);


		/// --------------------------------------------



		//int s = int(FWSync::ribbonStart)*mTessScene->mProfileNumIndices * 6;
		//int e = int(FWSync::ribbonEnd)**mTessScene->mProfileNumIndices * 6;
		//gl->setUniform(mMeteorRenderProgram->getUniformLoc("lastIndex"), float(e) / 6.0f);
		//glDrawElements(GL_TRIANGLES, e - s, GL_UNSIGNED_INT, NULL + (char*)(s * sizeof(GL_UNSIGNED_INT)));

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		mMeteorRenderProgram->use();
		gl->setUniform(mMeteorRenderProgram->getUniformLoc("toScreen"), camToClip);
		gl->setUniform(mMeteorRenderProgram->getUniformLoc("ribbonAlpha"), FWSync::simulationStep);
		gl->setUniform(mMeteorRenderProgram->getUniformLoc("cameraPosition"), cameraPos);
		int s = int(FWSync::ribbonStart)*mTessScene->mProfileNumIndices * 6;
		int e = int(FWSync::ribbonEnd)*mTessScene->mProfileNumIndices * 6;
		if (e - s > 0)
		{
			gl->setUniform(mMeteorRenderProgram->getUniformLoc("lastIndex"), float(e) / 6.0f);
			glBindVertexArray(mMeteorVAO);
			glDrawElements(GL_TRIANGLES, e - s, GL_UNSIGNED_INT, NULL + (char*)(s * sizeof(GL_UNSIGNED_INT)));
			glBindVertexArray(0);
		}
		glDisable(GL_BLEND);
		
		
		
		

		/// --------------------------------------------

		if (FWSync::godrayWeight > 0.0f)
		{
			godrayPass(gl, camToClip);
		}
		

		/// -------------------------------------------

		mGBuffer->unbind();

		glDisable(GL_DEPTH_TEST);

		mLastFBO->bind();

		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		mCombineProgram->use();

		gl->setUniform(mCombineProgram->getUniformLoc("meshColorTex"), 0);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, mGBuffer->getTexture(0));

		gl->setUniform(mCombineProgram->getUniformLoc("godrayColorTex"), 1);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, mGodrayBlurTex);
		
		gl->setUniform(mCombineProgram->getUniformLoc("useGodray"), FWSync::godrayWeight>0.0f);

		glBindVertexArray(mTessScene->m_quadVAO);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glBindVertexArray(0);

		//mTessScene->debugRenderPath(gl);

		mLastFBO->unbind();

		glEnable(GL_DEPTH_TEST);
	}

	void TunnelScene::generateTunnel()
	{
		std::vector<Vec3f> profilePoints = { Vec3f(0.000000,-0.250000, 0),
			Vec3f(0.000000, -0.250000, 0.0),
			Vec3f(0.550000, -0.550000, 0.0),
			Vec3f(0.250000, 0.000000, 0.0),
			Vec3f(0.550000, 0.550000, 0.0),
			Vec3f(-0.550000, 0.550000, 0.0),
			Vec3f(-0.250000, 0.000000, 0.0),
			Vec3f(0.000000, -0.250000, 0.0),
			Vec3f(0.550000, -0.550000, 0.0),
			Vec3f(0.250000, 0.000000, 0.0) };

		

		for (size_t i = 0; i < profilePoints.size(); ++i) profilePoints[i] *= 12.6f;

		Curve sweepCurve = evalTrefoilKnot(400.0);
		for (size_t i = 0; i < sweepCurve.size(); ++i) {
			sweepCurve[i].V *= 16.0f;
		}
		Curve profileCurve = evalBspline(profilePoints, 60.0, false, 0.0, 0.0);
		Surface tunnelSurface = makeGenCyl(profileCurve, sweepCurve);

		mTunnelNumIndices = tunnelSurface.VF.size() * 3;
		std::vector<MissileMeshVertex> vertexData(tunnelSurface.VV.size());

		for (size_t k = 0; k < vertexData.size(); ++k)
		{
			vertexData[k] = MissileMeshVertex(tunnelSurface.VV[k], tunnelSurface.VN[k].normalized(), tunnelSurface.VT[k].x, tunnelSurface.VT[k].y);
		}
		glGenVertexArrays(1, &mTunnelVAO);
		glBindVertexArray(mTunnelVAO);

		glGenBuffers(1, &mTunnelIBO);
		glGenBuffers(1, &mTunnelVBO);

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mTunnelIBO);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, tunnelSurface.VF.size() * sizeof(Vec3i), tunnelSurface.VF.data(), GL_STATIC_DRAW);

		glBindBuffer(GL_ARRAY_BUFFER, mTunnelVBO);
		glBufferData(GL_ARRAY_BUFFER, vertexData.size() * sizeof(MissileMeshVertex), vertexData.data(), GL_STATIC_DRAW);

		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(MissileMeshVertex), NULL);

		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(MissileMeshVertex), (char*)NULL + sizeof(Vec4f));

		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glBindVertexArray(0);

	}

	void TunnelScene::setupGLBuffers() {
		const static F32 posAttrib[] =
		{
			-1, -1, 0, 1,
			1, -1, 0, 1,
			-1, 1, 0, 1,
			1, 1, 0, 1
		};

		glGenVertexArrays(1, &mQuadVAO);
		glGenBuffers(1, &mQuadVBO);
		glBindVertexArray(mQuadVAO);
		glBindBuffer(GL_ARRAY_BUFFER, mQuadVBO);
		glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 16, posAttrib, GL_STATIC_DRAW);

		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);

		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glBindVertexArray(0);
	}

	void TunnelScene::handleAction(const Action & action, Window & wnd, CommonControls & controls) {
		Window::Event ev;

		switch (action) {

		case Action::Action_ReloadShaders:
			loadShaders(wnd.getGL());
			break;
		case Action::Action_SaveRibbonPath:
			mTessScene->saveRibbonPaths();
			break;
		case Action::Action_RightButton:
			mTessScene->selectControlPoint(wnd.getMousePos());
			break;
		case Action::Action_RestartAnimation:
			restartAnimation();
			break;
		default:
			break;
		}

	}

	void TunnelScene::loadShaders(GLContext * ctx) {


		mCombineProgram = loadShader(ctx, "shaders/tunnel/combine_vertex.glsl", "shaders/tunnel/combine_fragment.glsl", "tunnel_combine");
		mDisplayProgram = loadShader(ctx, "shaders/common/display_vertex.glsl", "shaders/common/display_fragment.glsl", "tunnel_display_program");

		mCityRenderProgram = loadShader(ctx, "shaders/particle_city/render_vert.glsl", "shaders/particle_city/render_frag.glsl", "particle_city_render..");
		mCityLightRenderProgram = loadShader(ctx, "shaders/particle_city/render_light_vert.glsl", "shaders/particle_city/render_light_frag.glsl", "particle_city_light_render..");

		mMeteorRenderProgram = loadShader(ctx, "shaders/particle_city/mesh_curve_vert.glsl", "shaders/particle_city/mesh_curve_frag.glsl", "particle_city_ribbon_meteor");

		mParticleMoveProgram = loadShader(ctx, "shaders/particle_city/move_particles.glsl", "mesh_city_move_particles");

		mGodrayBlurProgram = loadShader(ctx, "shaders/common/display_vertex.glsl",
			"shaders/particle_city/godray_blur_frag.glsl", "godray_blur_city_particle");

		mBackgroundRenderProgram = loadShader(ctx, "shaders/common/display_vertex.glsl", "shaders/particle_city/background.glsl", "particle_city_background");
	}


	void TunnelScene::activate(Window & wnd, CommonControls & controls) {

		updateGUI(wnd, controls);

	}

	void TunnelScene::cleanUpGUI(Window & wnd, CommonControls & controls) {
		// remove all knobs
		for (int i = 0; i < 10; ++i) {
			controls.removeControl(&m_knobs[i].x);
			controls.removeControl(&m_knobs[i].y);
			controls.removeControl(&m_knobs[i].z);
			controls.removeControl(&m_knobs[i].w);
		}

		// remove toggles
		controls.removeControl(&actionExt);
	}

	void TunnelScene::updateGUI(Window & wnd, CommonControls & controls) {

		/*cleanUpGUI(wnd, controls);

		controls.addSeparator();

		static const std::pair<Vec4f, Vec4f> KNOB_SLIDE_DATA[10] = {
			std::make_pair(Vec4f(0.0f), Vec4f(1.0f)), // knob1
			std::make_pair(Vec4f(0.0f), Vec4f(1.0f)), // knob2
			std::make_pair(Vec4f(0.0f), Vec4f(1.0f)), // knob3
			std::make_pair(Vec4f(0.0f), Vec4f(1.0f)), // knob4
			std::make_pair(Vec4f(-10.0f), Vec4f(10.0f)), // knob5
			std::make_pair(Vec4f(0.0f), Vec4f(10.0f)), // knob6
			std::make_pair(Vec4f(0.0f), Vec4f(10.0f)), // knob7
			std::make_pair(Vec4f(0.0f), Vec4f(10.0f)), // knob8
			std::make_pair(Vec4f(0.0f), Vec4f(1.0f)), // knob9
			std::make_pair(Vec4f(0.0f), Vec4f(1.0f)) // knob10
		};


		String xStr = sprintf("Knob%u.x = %%f", activeKnob + 1);
		String yStr = sprintf("Knob%u.y = %%f", activeKnob + 1);
		String zStr = sprintf("Knob%u.z = %%f", activeKnob + 1);
		String wStr = sprintf("Knob%u.w = %%f", activeKnob + 1);

		controls.beginSliderStack();
		controls.addSlider(&m_knobs[activeKnob].x, KNOB_SLIDE_DATA[activeKnob].first.x, KNOB_SLIDE_DATA[activeKnob].second.x, false, FW_KEY_NONE, FW_KEY_NONE, xStr);
		controls.addSlider(&m_knobs[activeKnob].y, KNOB_SLIDE_DATA[activeKnob].first.y, KNOB_SLIDE_DATA[activeKnob].second.y, false, FW_KEY_NONE, FW_KEY_NONE, yStr);
		controls.addSlider(&m_knobs[activeKnob].z, KNOB_SLIDE_DATA[activeKnob].first.z, KNOB_SLIDE_DATA[activeKnob].second.z, false, FW_KEY_NONE, FW_KEY_NONE, zStr);
		controls.addSlider(&m_knobs[activeKnob].w, KNOB_SLIDE_DATA[activeKnob].first.w, KNOB_SLIDE_DATA[activeKnob].second.w, false, FW_KEY_NONE, FW_KEY_NONE, wStr);
		controls.endSliderStack();*/

		mTessScene->updateGUI(wnd, controls);

	}

	Vec3f TunnelScene::getCameraPosition() {

		int camIndex = FWSync::cameraIndex;
		if (camIndex == 6)
		{

			// curve path
			float t = FWSync::cameraTime;
			int idx = int(t);

			t -= float(idx);
			if (idx + 3 >= mCameraPaths[camIndex].size()) {
				idx = mCameraPaths[camIndex].size() - 4;
				t = 1.0f;
			}
			return CarmullRomCurve::evalCatmullRom(
				mCameraPaths[camIndex][idx],
				mCameraPaths[camIndex][idx + 1],
				mCameraPaths[camIndex][idx + 2],
				mCameraPaths[camIndex][idx + 3], t);
		}
		else if (camIndex == 0)
		{
			float r = FWSync::sphereR;
			float a = FWSync::sphereAlpha * FW_PI / 180.0f;
			float th = FWSync::splhereTheta * FW_PI / 180.0f;
			float x = r * cosf(th) *sinf(a);
			float y = r * sinf(th) * sinf(a);
			float z = r * cosf(a);
			return Vec3f(x, y, z);
		}
		else if (camIndex == 1)
		{
			float r = FWSync::sphereR;
			float a = FWSync::sphereAlpha * FW_PI / 180.0f;
			float th = FWSync::splhereTheta * FW_PI / 180.0f;
			float x = r * cosf(th);
			float y = FWSync::sphereAlpha;
			float z = r * sin(th);
			return Vec3f(x, y, z);
		}
		else
		{
			return Vec3f(0.0f);
		}
			

	}

	Vec3f TunnelScene::getCameraForward() {


		return normalize(Vec3f(FWSync::lookAtX, FWSync::lookAtY, FWSync::lookAtZ) - mCamPtr->getPosition());

	}

	void TunnelScene::loadCamPaths()
	{
		size_t numCamPaths = 8;
		mCameraPaths.resize(numCamPaths);
		for (size_t i = 0; i < numCamPaths; ++i)
		{
			std::string filePath = "assets/particle_city/cam_path_water_" + std::to_string(i + 1) + ".txt";
			mTessScene->loadCamPath(filePath, mCameraPaths[i]);
		}
	}

	Vec3f TunnelScene::getCameraUp(float t)
	{
		t += 0.1f;
		return (3200.0f*Vec3f(
			0.5f * (-cosf(t) - 16.0f * cosf(2.0f * t) - 25.0f * cosf(5.0f * t)),
			0.5f * (sinf(t) - 16.0f * sinf(2.0f * t) - 25.0f * sinf(5.0f * t)),
			-9.0f * sinf(3.0f * t))).normalized();
	}

	void TunnelScene::renderCity(GLContext * gl, const Mat4f & toScreen, const Vec3f & lightPosition, const Vec3f & lightDirection, const Vec3f & lightColor, const Vec3f & fogColor)
	{
		Mat4f toWorld = mTessScene->getCityMeshWorldMatrix();

		mCityRenderProgram->use();
		gl->setUniform(mCityRenderProgram->getUniformLoc("toScreen"), toScreen*toWorld);
		gl->setUniform(mCityRenderProgram->getUniformLoc("posToWorld"), toWorld);
		gl->setUniform(mCityRenderProgram->getUniformLoc("normalToWorld"), toWorld.inverted().transposed());
		gl->setUniform(mCityRenderProgram->getUniformLoc("lightPos"), lightPosition);
		gl->setUniform(mCityRenderProgram->getUniformLoc("lightDirection"), lightDirection);
		gl->setUniform(mCityRenderProgram->getUniformLoc("cameraPosition"), mCamPtr->getPosition());
		gl->setUniform(mCityRenderProgram->getUniformLoc("lightColor"), lightColor);
		gl->setUniform(mCityRenderProgram->getUniformLoc("fogColor"), fogColor);
		gl->setUniform(mCityRenderProgram->getUniformLoc("particleSize"), FWSync::particleSize);
		gl->setUniform(mCityRenderProgram->getUniformLoc("particleDim"), FWSync::particleDim);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, mBokehTexture);
		glActiveTexture(GL_TEXTURE0 + 1);
		glBindTexture(GL_TEXTURE_2D, mBokehTextureStrip);

		gl->setUniform(mCityRenderProgram->getUniformLoc("bokehTexture"), 0);
		gl->setUniform(mCityRenderProgram->getUniformLoc("bokehTextureStrip"), 1);

		for (size_t i = 0; i < mTextureHandles.size(); ++i) {
			if (!glIsTextureHandleResidentARB(mTextureHandles[i])) glMakeTextureHandleResidentARB(mTextureHandles[i]);
			// bound to unit
			char buf[32];
			sprintf_s(buf, "diffuseSamplers[%d]", int(i));
			GLint loc = mCityRenderProgram->getUniformLoc(buf);
			glUniformHandleui64ARB(loc, mTextureHandles[i]);
		}

		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, mParticleMaterialSSBO);

		glBindVertexArray(mCityVAO);
		glEnable(GL_POINT_SPRITE);
		glDisable(GL_POINT_SMOOTH);
		glDepthMask(GL_FALSE);
		glDisable(GL_DEPTH_TEST);
		glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
		glEnable(GL_BLEND);
		//glBlendFunc(GL_SRC_ALPHA, GL_ONE); // additive blending
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // additive blending
		glDrawArrays(GL_POINTS, 0, mNumCityParticles);
		glDepthMask(GL_TRUE);
		glEnable(GL_DEPTH_TEST);
		glDisable(GL_BLEND);
		glBindVertexArray(0);

		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);

		for (size_t i = 0; i < mTextureHandles.size(); ++i) {
			if (glIsTextureHandleResidentARB(mTextureHandles[i])) {
				glMakeTextureHandleNonResidentARB(mTextureHandles[i]);
			}
		}
	}

	void TunnelScene::explodeCity(GLContext * gl)
	{

		static float lastValue = FWSync::trefoilTime;

		if (lastValue + 0.5f < FWSync::trefoilTime)
		{
			restartAnimation();
		}
		lastValue = FWSync::trefoilTime;

		mParticleMoveProgram->use();

		gl->setUniform(mParticleMoveProgram->getUniformLoc("numParticles"), mNumCityParticles);

		gl->setUniform(mParticleMoveProgram->getUniformLoc("dtUniform"), GLOBAL_DT);
		gl->setUniform(mParticleMoveProgram->getUniformLoc("offset"), 0);

		gl->setUniform(mParticleMoveProgram->getUniformLoc("curlStep"), FWSync::cloudParticleCurlStep);
		gl->setUniform(mParticleMoveProgram->getUniformLoc("attractorStep"), FWSync::cloudParticleStep);
		gl->setUniform(mParticleMoveProgram->getUniformLoc("invocationModulate"), FWSync::invocationModulate);
		gl->setUniform(mParticleMoveProgram->getUniformLoc("invocationScale"), FWSync::invocationScale);

		gl->setUniform(mParticleMoveProgram->getUniformLoc("attractorPosition"), Vec3f(FWSync::attrX, FWSync::attrY, FWSync::attrZ));

		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, mCityVBO);

		int localSizeX = 128;

		int numParticlesToUpdate = int(FWSync::pUpdateTo);

		int groupSizeX = (mNumCityParticles + localSizeX - 1) / localSizeX;

		glDispatchCompute(groupSizeX, 1, 1);

		glMemoryBarrier(GL_ALL_BARRIER_BITS);

		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
	}
	
	void TunnelScene::godrayPass(GLContext * gl, const Mat4f & toScreen)
	{
		mGodrayBlurFBO->bind();

		mGodrayBlurProgram->use();

		Vec4f lightClipSpace = toScreen * Vec4f(0, 0, 0, 1.0f);
		lightClipSpace /= lightClipSpace.w;
		lightClipSpace.x = lightClipSpace.x*0.5 + 0.5;
		lightClipSpace.y = lightClipSpace.y*0.5 + 0.5;

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, mGodrayFBO->getTexture(0));

		gl->setUniform(mGodrayBlurProgram->getUniformLoc("bwSmapler"), 0);
		gl->setUniform(mGodrayBlurProgram->getUniformLoc("lightPos"), lightClipSpace.getXY());
		gl->setUniform(mGodrayBlurProgram->getUniformLoc("WEIGHT"), FWSync::godrayWeight);
		gl->setUniform(mGodrayBlurProgram->getUniformLoc("time"), GLOBAL_DT);

		glDisable(GL_DEPTH_TEST);
		glBindVertexArray(mQuadVAO);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glBindVertexArray(0);
		glEnable(GL_DEPTH_TEST);

		mGodrayBlurFBO->unbind();



		mGaussiaFilter->process(gl, mGodrayBlurFBO->getTexture(0), mGodrayBlurTex, 2);
	}

	void TunnelScene::restartAnimation()
	{
		glBindBuffer(GL_COPY_READ_BUFFER, mCityVBOCopy);
		glBindBuffer(GL_COPY_WRITE_BUFFER, mCityVBO);
		glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, sizeof(ParticleInfo) * mNumCityParticles);
	}

};