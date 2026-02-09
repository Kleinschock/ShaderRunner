#version 330 core

// Fullscreen quad vertex shader for post-processing
// Expects a unit quad [0,1]x[0,1] with tex coords

layout(location = 0) in vec4 osg_Vertex;
layout(location = 1) in vec4 osg_MultiTexCoord0;

out vec2 v_texCoord;

uniform mat4 osg_ModelViewProjectionMatrix;

void main()
{
    v_texCoord  = osg_MultiTexCoord0.xy;
    gl_Position = osg_ModelViewProjectionMatrix * osg_Vertex;
}
