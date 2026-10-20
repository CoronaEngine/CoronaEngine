#version 450
#extension GL_EXT_nonuniform_qualifier : enable
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 3, binding = 0, rgba16) uniform image2D inputImageRGBA16[];

layout(push_constant) uniform PushConsts
{
    uint uniformBufferIndex;
} pushConsts;

layout(set = 0, binding = 0) uniform UniformBufferObject
{
    float time;
    vec2 imageSize;
    uint imageID;
}uniformBufferObjects[];

vec3 acesFilmicToneMapCurve(vec3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;

    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 acesFilmicToneMapInverse(vec3 x)
{
    vec3 a = -0.59 * x + 0.03;
    vec3 b = sqrt(-1.0127 * x * x + 1.3702 * x + 0.0009);
    vec3 c = 2 * (2.43 * x - 2.51);
    return ((a - b) / c);
}
/*originals https://www.shadertoy.com/view/lXsSRN https://www.shadertoy.com/view/wdtczM*/
float happy_star(vec2 uv, float anim)
{
    uv = abs(uv);
    vec2 pos = min(uv.xy/uv.yx, anim);
    float p = (2.0 - pos.x - pos.y);
    return (2.0+p*(p*p-1.5)) / (uv.x+uv.y);
}

float hash( ivec3 p )    // this hash is not production ready, please
{                        // replace this by something better

    // 3D -> 1D
    int n = p.x*3 + p.y*113 + p.z*311;

    // 1D hash by Hugo Elias
	n = (n << 13) ^ n;
    n = n * (n * n * 15731 + 789221) + 1376312589;
    return float( n & ivec3(0x0fffffff))/float(0x0fffffff);
}
vec2 rotZ( float alpha, vec2 r)
{
    float xComp = r.x*cos(alpha) - r.y*sin(alpha);
    float yComp = r.x*sin(alpha) + r.y*cos(alpha);
    return vec2( xComp, yComp );
}
float noise(vec3 x ,float iTime)
{
x.xz=rotZ(iTime, x.zx);
    ivec3 i = ivec3(floor(x));
    vec3 f = fract(x);
    f = f*f*(3.0-2.0*f);

    return mix(mix(mix( hash(i+ivec3(0,0,0)),
                        hash(i+ivec3(1,0,0)),f.x),
                   mix( hash(i+ivec3(0,1,0)),
                        hash(i+ivec3(1,1,0)),f.x),f.y),
               mix(mix( hash(i+ivec3(0,0,1)),
                        hash(i+ivec3(1,0,1)),f.x),
                   mix( hash(i+ivec3(0,1,1)),
                        hash(i+ivec3(1,1,1)),f.x),f.y),f.z);
}

// https://www.shadertoy.com/view/XsGfWV
vec3 aces_tonemap(vec3 color){
	mat3 m1 = mat3(
        0.59719, 0.07600, 0.02840,
        0.35458, 0.90834, 0.13383,
        0.04823, 0.01566, 0.83777
	);
	mat3 m2 = mat3(
        1.60475, -0.10208, -0.00327,
        -0.53108,  1.10813, -0.07276,
        -0.07367, -0.00605,  1.07602
	);
	vec3 v = m1 * color;
	vec3 a = v * (v + 0.0245786) - 0.000090537;
	vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
	return pow(clamp(m2 * (a / b), 0.0, 1.0), vec3(1.0 / 2.2));
}

vec2 r(vec2 p, float a) { return p*mat2(cos(a), sin(a), -sin(a), cos(a)); }


void main()
{
    vec4 O=vec4(0.0);
    float iTime = uniformBufferObjects[pushConsts.uniformBufferIndex].time * 0.05;
    vec2 iResolution = uniformBufferObjects[pushConsts.uniformBufferIndex].imageSize;
    vec2 R = uniformBufferObjects[pushConsts.uniformBufferIndex].imageSize;
    vec2 I = vec2(gl_GlobalInvocationID.xy) + vec2(0.5);
    vec2 uv = I/R;
    vec2 p = (2.*I - R) / R.y * 1.5;

    float fp = pow(.5/length(pow(abs(r(p,.43))
        *vec2(3.4,1),vec2(0.5))),2.5);


       vec4 o=O;
     vec2 F = I;

    o-=o;
    for(float d,t = -iTime*.01, i = 0. ; i > -1.; i -= .06 )
    {   d = fract( i -3.*t );
        vec4 c = vec4( ( F - R *.5 ) / R.y *d ,i,0 ) * 28.;
     c.xz=rotZ(iTime, c.zx);
        for (int j=0 ; j++ <27; )
            c.xzyw = abs( c / dot(c,c)

                    -vec4( 7.-.2*sin(t) , 6.3 , .7 , 1.-cos(t/.8))/7.);

       o -= c * c.yzww  * d--*d  / vec4(3,4,1,1);
    }
    vec2 uv2 = ( I - .5*iResolution.xy ) / iResolution.y;
      vec2 uv3 = ( I - .5*iResolution.xy ) / iResolution.y;
         vec2 uv4 = ( I - .5*iResolution.xy ) / iResolution.y;
    p *= mat2(1.0,-.1,-.0,1.2);
    vec3 pos = normalize(vec3(r(p,-.4/length(p)),.25));
    pos.z -= iTime*0.5;
    uv2-=0.2;
       uv3+=0.1;
         uv4+=.3;
         uv4.y-=0.72;
    vec3 q = 2.*pos;

    float f  = 0.5000*noise( q*o.xyz ,iTime); q = q*2.;
          f += 0.2500*noise( q*o.xyz ,iTime); q = q*2.;
          f += 0.1250*noise( q *o.xyz,iTime); q = q*2.;
          f += 0.0625*noise( q *o.xyz,iTime);

    vec2 n = uv*(1.-uv+o.xy)*3.; float v = pow(n.x*n.y,.8);

    float fr = .6/length(p);
    f = smoothstep(-.4,2.,f*f) * fr*fr + fp;
     uv *= 2.0 * ( cos(iTime * 2.0) -2.5); // scale
    float anim = sin(iTime * 12.0) * 0.1 + 1.0;  // anim between 0.9 - 1.1

    O = vec4(pow(f*f * vec3(0., .05, .5)*v*o.xyz,vec3(.45))*3.5,0);

    O+= vec4(happy_star(uv2, anim) * vec3(0.35,0.2,0.15)*0.01, 1.0);
    O+= vec4(happy_star(uv3, anim) * vec3(0.35,0.7,0.35)*0.005, 1.0);
    O+= vec4(happy_star(uv4, anim) * vec3(0.35,0.07,0.05)*0.0205, 1.0);

    uint imageID = uniformBufferObjects[pushConsts.uniformBufferIndex].imageID;
    imageStore(inputImageRGBA16[imageID], ivec2(gl_GlobalInvocationID.xy), vec4(acesFilmicToneMapCurve(O.xyz), 1.0));
}