#pragma once
// ============================================================================
//  PhotonNoiseEffect — Poisson shot noise module
// ============================================================================

#include "INoiseEffect.h"
#include <osg/Uniform>
#include <fstream>
#include <sstream>
#include <iostream>

class PhotonNoiseEffect : public INoiseEffect
{
public:
    PhotonNoiseEffect(const std::string& shaderDir = "shaders",
                      float photonScale = 100.0f)
        : m_shaderDir(shaderDir), m_photonScale(photonScale)
    {
        m_uniformPhotonScale = new osg::Uniform("u_photonScale", m_photonScale);
        m_uniformFrameNumber = new osg::Uniform("u_frameNumber", 0);
        m_uniformResolution  = new osg::Uniform("u_resolution", osg::Vec2(1280, 720));
    }

    std::string getName() const override { return "PhotonNoise"; }

    std::string getFragmentSource() const override
    {
        std::ifstream ifs(m_shaderDir + "/photon_noise.frag");
        if (!ifs.is_open()) return {};
        std::stringstream ss; ss << ifs.rdbuf();
        return ss.str();
    }

    void setupUniforms(osg::StateSet* ss) override
    {
        ss->addUniform(m_uniformPhotonScale);
        ss->addUniform(m_uniformFrameNumber);
        ss->addUniform(m_uniformResolution);
    }

    osg::ref_ptr<osg::NodeCallback> createUpdateCallback() override;

    // ── Parameter access ────────────────────────────────────────────────
    void setPhotonScale(float s)  { m_photonScale = std::max(1.0f, s); m_uniformPhotonScale->set(m_photonScale); }
    float getPhotonScale() const  { return m_photonScale; }

    void setResolution(float w, float h) { m_uniformResolution->set(osg::Vec2(w, h)); }

private:
    std::string m_shaderDir;
    float m_photonScale;
    osg::ref_ptr<osg::Uniform> m_uniformPhotonScale;
    osg::ref_ptr<osg::Uniform> m_uniformFrameNumber;
    osg::ref_ptr<osg::Uniform> m_uniformResolution;
};

// ── Frame counter callback ──────────────────────────────────────────────
class PhotonNoiseFrameCallback : public osg::NodeCallback
{
public:
    PhotonNoiseFrameCallback(osg::ref_ptr<osg::Uniform> u) : m_u(u), m_f(0) {}
    void operator()(osg::Node* node, osg::NodeVisitor* nv) override
    {
        m_u->set(m_f++);
        traverse(node, nv);
    }
private:
    osg::ref_ptr<osg::Uniform> m_u;
    int m_f;
};

inline osg::ref_ptr<osg::NodeCallback> PhotonNoiseEffect::createUpdateCallback()
{
    return new PhotonNoiseFrameCallback(m_uniformFrameNumber);
}
