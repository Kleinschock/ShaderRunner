#pragma once
// ============================================================================
//  DarkNoiseEffect — Dark current + DSNU + hot pixels module
// ============================================================================

#include "INoiseEffect.h"
#include <osg/Uniform>
#include <fstream>
#include <sstream>
#include <algorithm>

class DarkNoiseEffect : public INoiseEffect
{
public:
    DarkNoiseEffect(const std::string& shaderDir = "shaders",
                    float darkCurrent = 0.005f,
                    float dsnuStrength = 0.003f,
                    float hotPixelProb = 0.0005f,
                    float hotPixelStr  = 50.0f)
        : m_shaderDir(shaderDir)
        , m_darkCurrent(darkCurrent)
        , m_dsnuStrength(dsnuStrength)
        , m_hotPixelProbability(hotPixelProb)
        , m_hotPixelStrength(hotPixelStr)
    {
        m_uDarkCurrent  = new osg::Uniform("u_darkCurrent", m_darkCurrent);
        m_uDSNU         = new osg::Uniform("u_dsnuStrength", m_dsnuStrength);
        m_uHotPixelProb = new osg::Uniform("u_hotPixelProbability", m_hotPixelProbability);
        m_uHotPixelStr  = new osg::Uniform("u_hotPixelStrength", m_hotPixelStrength);
        m_uFrameNumber  = new osg::Uniform("u_frameNumber", 0);
        m_uResolution   = new osg::Uniform("u_resolution", osg::Vec2(1280, 720));
    }

    std::string getName() const override { return "DarkNoise"; }

    std::string getFragmentSource() const override
    {
        std::ifstream ifs(m_shaderDir + "/dark_noise.frag");
        if (!ifs.is_open()) return {};
        std::stringstream ss; ss << ifs.rdbuf();
        return ss.str();
    }

    void setupUniforms(osg::StateSet* ss) override
    {
        ss->addUniform(m_uDarkCurrent);
        ss->addUniform(m_uDSNU);
        ss->addUniform(m_uHotPixelProb);
        ss->addUniform(m_uHotPixelStr);
        ss->addUniform(m_uFrameNumber);
        ss->addUniform(m_uResolution);
    }

    osg::ref_ptr<osg::NodeCallback> createUpdateCallback() override;

    // ── Parameter access ────────────────────────────────────────────────
    void  setDarkCurrent(float v)       { m_darkCurrent = std::max(0.f, v);          m_uDarkCurrent->set(m_darkCurrent); }
    float getDarkCurrent() const        { return m_darkCurrent; }

    void  setDSNUStrength(float v)      { m_dsnuStrength = std::max(0.f, v);         m_uDSNU->set(m_dsnuStrength); }
    float getDSNUStrength() const       { return m_dsnuStrength; }

    void  setHotPixelProbability(float v){ m_hotPixelProbability = std::clamp(v,0.f,1.f); m_uHotPixelProb->set(m_hotPixelProbability); }
    float getHotPixelProbability() const { return m_hotPixelProbability; }

    void  setHotPixelStrength(float v)  { m_hotPixelStrength = std::max(0.f, v);     m_uHotPixelStr->set(m_hotPixelStrength); }
    float getHotPixelStrength() const   { return m_hotPixelStrength; }

    void setResolution(float w, float h){ m_uResolution->set(osg::Vec2(w, h)); }

private:
    std::string m_shaderDir;
    float m_darkCurrent, m_dsnuStrength, m_hotPixelProbability, m_hotPixelStrength;

    osg::ref_ptr<osg::Uniform> m_uDarkCurrent;
    osg::ref_ptr<osg::Uniform> m_uDSNU;
    osg::ref_ptr<osg::Uniform> m_uHotPixelProb;
    osg::ref_ptr<osg::Uniform> m_uHotPixelStr;
    osg::ref_ptr<osg::Uniform> m_uFrameNumber;
    osg::ref_ptr<osg::Uniform> m_uResolution;
};

// ── Frame counter callback ──────────────────────────────────────────────
class DarkNoiseFrameCallback : public osg::NodeCallback
{
public:
    DarkNoiseFrameCallback(osg::ref_ptr<osg::Uniform> u) : m_u(u), m_f(0) {}
    void operator()(osg::Node* node, osg::NodeVisitor* nv) override
    {
        m_u->set(m_f++);
        traverse(node, nv);
    }
private:
    osg::ref_ptr<osg::Uniform> m_u;
    int m_f;
};

inline osg::ref_ptr<osg::NodeCallback> DarkNoiseEffect::createUpdateCallback()
{
    return new DarkNoiseFrameCallback(m_uFrameNumber);
}
