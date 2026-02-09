#pragma once
// ============================================================================
//  ReadNoiseEffect — Gaussian readout noise module
// ============================================================================

#include "INoiseEffect.h"
#include <osg/Uniform>
#include <fstream>
#include <sstream>
#include <algorithm>

class ReadNoiseEffect : public INoiseEffect
{
public:
    ReadNoiseEffect(const std::string& shaderDir = "shaders",
                    float readNoise = 0.01f)
        : m_shaderDir(shaderDir), m_readNoise(readNoise)
    {
        m_uReadNoise   = new osg::Uniform("u_readNoise", m_readNoise);
        m_uFrameNumber = new osg::Uniform("u_frameNumber", 0);
        m_uResolution  = new osg::Uniform("u_resolution", osg::Vec2(1280, 720));
    }

    std::string getName() const override { return "ReadNoise"; }

    std::string getFragmentSource() const override
    {
        std::ifstream ifs(m_shaderDir + "/read_noise.frag");
        if (!ifs.is_open()) return {};
        std::stringstream ss; ss << ifs.rdbuf();
        return ss.str();
    }

    void setupUniforms(osg::StateSet* ss) override
    {
        ss->addUniform(m_uReadNoise);
        ss->addUniform(m_uFrameNumber);
        ss->addUniform(m_uResolution);
    }

    osg::ref_ptr<osg::NodeCallback> createUpdateCallback() override;

    // ── Parameter access ────────────────────────────────────────────────
    void  setReadNoise(float v) { m_readNoise = std::max(0.f, v); m_uReadNoise->set(m_readNoise); }
    float getReadNoise() const  { return m_readNoise; }

    void setResolution(float w, float h) { m_uResolution->set(osg::Vec2(w, h)); }

private:
    std::string m_shaderDir;
    float m_readNoise;
    osg::ref_ptr<osg::Uniform> m_uReadNoise;
    osg::ref_ptr<osg::Uniform> m_uFrameNumber;
    osg::ref_ptr<osg::Uniform> m_uResolution;
};

class ReadNoiseFrameCallback : public osg::NodeCallback
{
public:
    ReadNoiseFrameCallback(osg::ref_ptr<osg::Uniform> u) : m_u(u), m_f(0) {}
    void operator()(osg::Node* node, osg::NodeVisitor* nv) override
    {
        m_u->set(m_f++);
        traverse(node, nv);
    }
private:
    osg::ref_ptr<osg::Uniform> m_u;
    int m_f;
};

inline osg::ref_ptr<osg::NodeCallback> ReadNoiseEffect::createUpdateCallback()
{
    return new ReadNoiseFrameCallback(m_uFrameNumber);
}
