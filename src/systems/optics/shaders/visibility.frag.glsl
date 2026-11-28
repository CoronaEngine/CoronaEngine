#version 460
layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) flat in uint v_instanceID;

layout(location = 0) out uvec4 visibilityData;

void main()
{
    visibilityData = uvec4(v_instanceID, uint(gl_PrimitiveID), 0u, 0u);
}
