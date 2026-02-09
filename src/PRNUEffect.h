#pragma once
// ============================================================================
//  PRNUEffect — Photo-Response Non-Uniformity module
// ============================================================================

#include "INoiseEffect.h"
#include <osg/Uniform>
#include <fstream>
#include <sstream>
#include <algorithm>

class PRNUEffect : public INoiseEffect
{
public:
    PRNUEffect(const std::string& shaderDir = "shaders",
               float prnuStrength = 0.01f)
        : m_shaderDir(shaderDir), m_prnuStrength(prnuStrength)
    {
        m_uPRNU       = new osg::Uniform("u_prnuStrength", m_prnuStrength);
        m_uResolution = new osg::Uniform("u_resolution", osg::Vec2(1280, 720));
    }

    std::string getName() const override { return "PRNU"; }

    std::string getFragmentSource() const override
    {
        std::ifstream ifs(m_shaderDir + "/prnu.frag");
        if (!ifs.is_open()) return {};
        std::stringstream ss; ss << ifs.rdbuf();
        return ss.str();
    }

    void setupUniforms(osg::StateSet* ss) override
    {
        ss->addUniform(m_uPRNU);
        ss->addUniform(m_uResolution);
    }

    // PRNU has no temporal component — no update callback needed.

    // ── Parameter access ────────────────────────────────────────────────
    void  setPRNUStrength(float v) { m_prnuStrength = std::max(0.f, v); m_uPRNU->set(m_prnuStrength); }
    float getPRNUStrength() const  { return m_prnuStrength; }

    void setResolution(float w, float h) { m_uResolution->set(osg::Vec2(w, h)); }

private:
    std::string m_shaderDir;
    float m_prnuStrength;
    osg::ref_ptr<osg::Uniform> m_uPRNU;
    osg::ref_ptr<osg::Uniform> m_uResolution;
};
