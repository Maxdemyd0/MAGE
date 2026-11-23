// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
//
// Physically based atmosphere (after Hillaire 2020, "A Scalable and Production Ready Sky and
// Atmosphere Rendering Technique"): Rayleigh + Mie scattering, ozone absorption, planet shadow,
// ground bounce and a multiple-scattering approximation. Distances are in kilometres, the planet
// centre is at the origin and +Y is up. Requires common.glsl.

const ivec2 TRANSMITTANCE_LUT_SIZE = ivec2(256, 64);
const ivec2 MULTISCATTER_LUT_SIZE = ivec2(32, 32);
const ivec2 SKYVIEW_LUT_SIZE = ivec2(192, 108);

float BottomRadius() { return ubo.planet.x; }
float TopRadius() { return ubo.planet.y; }

// Camera position in planet space (km).
vec3 CameraAtmospherePos() {
    return vec3(0.0, BottomRadius() + max(ubo.planet.z + ubo.cameraPos.y * 0.001, 0.0005), 0.0);
}

// Nearest positive intersection with a sphere centred at the origin, or -1.
float RaySphere(vec3 ro, vec3 rd, float radius) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0) return -1.0;
    float s = sqrt(disc);
    float t0 = -b - s, t1 = -b + s;
    if (t0 >= 0.0) return t0;
    if (t1 >= 0.0) return t1;
    return -1.0;
}

struct Medium {
    vec3 scattering;
    vec3 extinction;
    vec3 rayleighScattering;
    vec3 mieScattering;
};

Medium SampleMedium(vec3 p) {
    float h = max(length(p) - BottomRadius(), 0.0);
    float densityR = exp(-h / ubo.rayleigh.w);
    // Aerosols: exponential profile scaled by the haze (turbidity) multiplier.
    float densityM = exp(-h / ubo.mie.w) * ubo.planet.w;
    float densityO = max(0.0, 1.0 - abs(h - 25.0) / 15.0);
    Medium m;
    m.rayleighScattering = ubo.rayleigh.rgb * densityR;
    m.mieScattering = vec3(ubo.mie.x * densityM);
    vec3 mieExtinction = vec3(ubo.mie.y * densityM);
    vec3 ozoneAbsorption = ubo.ozone.rgb * densityO;
    m.scattering = m.rayleighScattering + m.mieScattering;
    m.extinction = m.rayleighScattering + mieExtinction + ozoneAbsorption;
    return m;
}

float RayleighPhase(float cosTheta) { return 3.0 / (16.0 * PI) * (1.0 + cosTheta * cosTheta); }

float MiePhase(float cosTheta) { // Cornette-Shanks
    float g = ubo.mie.z;
    float k = 3.0 / (8.0 * PI) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + cosTheta * cosTheta) / pow(max(1.0 + g * g - 2.0 * g * cosTheta, 1e-4), 1.5);
}

// ---------------------------------------------------------------------------- LUT parameterisations
float FromUnitToSubUv(float u, float res) { return (u + 0.5 / res) * (res / (res + 1.0)); }
float FromSubUvToUnit(float u, float res) { return (u - 0.5 / res) * (res / (res - 1.0)); }

// Transmittance LUT (Bruneton): x = view zenith cosine, y = height.
vec2 TransmittanceParamsToUv(float viewHeight, float viewZenithCos) {
    float H = sqrt(max(0.0, TopRadius() * TopRadius() - BottomRadius() * BottomRadius()));
    float rho = sqrt(max(0.0, viewHeight * viewHeight - BottomRadius() * BottomRadius()));
    float disc = viewHeight * viewHeight * (viewZenithCos * viewZenithCos - 1.0) + TopRadius() * TopRadius();
    float d = max(0.0, -viewHeight * viewZenithCos + sqrt(max(disc, 0.0)));
    float dMin = TopRadius() - viewHeight;
    float dMax = rho + H;
    return vec2((d - dMin) / max(dMax - dMin, 1e-6), rho / H);
}

void UvToTransmittanceParams(vec2 uv, out float viewHeight, out float viewZenithCos) {
    float H = sqrt(max(0.0, TopRadius() * TopRadius() - BottomRadius() * BottomRadius()));
    float rho = H * uv.y;
    viewHeight = sqrt(rho * rho + BottomRadius() * BottomRadius());
    float dMin = TopRadius() - viewHeight;
    float dMax = rho + H;
    float d = dMin + uv.x * (dMax - dMin);
    viewZenithCos = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * viewHeight * d);
    viewZenithCos = clamp(viewZenithCos, -1.0, 1.0);
}

vec3 SampleTransmittance(float viewHeight, float viewZenithCos) {
    return texture(lutTex[0], TransmittanceParamsToUv(viewHeight, viewZenithCos)).rgb;
}

vec3 SampleMultiScattering(float viewHeight, float sunZenithCos) {
    vec2 uv = vec2(clamp(sunZenithCos * 0.5 + 0.5, 0.0, 1.0),
                   clamp((viewHeight - BottomRadius()) / (TopRadius() - BottomRadius()), 0.0, 1.0));
    uv = vec2(FromUnitToSubUv(uv.x, float(MULTISCATTER_LUT_SIZE.x)), FromUnitToSubUv(uv.y, float(MULTISCATTER_LUT_SIZE.y)));
    return texture(lutTex[1], uv).rgb;
}

// Sky-view LUT (latitude/longitude around the sun azimuth, horizon-weighted).
vec2 SkyViewParamsToUv(bool intersectGround, float viewZenithCos, float lightViewCos, float viewHeight) {
    float vHorizon = sqrt(max(viewHeight * viewHeight - BottomRadius() * BottomRadius(), 0.0));
    float beta = acos(clamp(vHorizon / viewHeight, -1.0, 1.0));
    float zenithHorizonAngle = PI - beta;
    vec2 uv;
    if (!intersectGround) {
        float coord = acos(clamp(viewZenithCos, -1.0, 1.0)) / zenithHorizonAngle;
        coord = 1.0 - sqrt(max(1.0 - coord, 0.0));
        uv.y = coord * 0.5;
    } else {
        float coord = (acos(clamp(viewZenithCos, -1.0, 1.0)) - zenithHorizonAngle) / beta;
        uv.y = sqrt(max(coord, 0.0)) * 0.5 + 0.5;
    }
    uv.x = sqrt(clamp(-lightViewCos * 0.5 + 0.5, 0.0, 1.0));
    return vec2(FromUnitToSubUv(uv.x, float(SKYVIEW_LUT_SIZE.x)), FromUnitToSubUv(uv.y, float(SKYVIEW_LUT_SIZE.y)));
}

void UvToSkyViewParams(vec2 uv, float viewHeight, out float viewZenithCos, out float lightViewCos) {
    uv = vec2(FromSubUvToUnit(uv.x, float(SKYVIEW_LUT_SIZE.x)), FromSubUvToUnit(uv.y, float(SKYVIEW_LUT_SIZE.y)));
    float vHorizon = sqrt(max(viewHeight * viewHeight - BottomRadius() * BottomRadius(), 0.0));
    float beta = acos(clamp(vHorizon / viewHeight, -1.0, 1.0));
    float zenithHorizonAngle = PI - beta;
    if (uv.y < 0.5) {
        float coord = 1.0 - 2.0 * uv.y;
        coord = 1.0 - coord * coord;
        viewZenithCos = cos(zenithHorizonAngle * coord);
    } else {
        float coord = uv.y * 2.0 - 1.0;
        viewZenithCos = cos(zenithHorizonAngle + beta * coord * coord);
    }
    float c = uv.x * uv.x;
    lightViewCos = -(c * 2.0 - 1.0);
}

// ---------------------------------------------------------------------------- integration
struct ScatteringResult {
    vec3 luminance;       // in-scattered radiance
    vec3 transmittance;   // along the segment
    vec3 multiScatAs1;    // f_ms integrand (multiple-scattering LUT only)
};

// Ray-marches single scattering (+ optional multiple-scattering term) from 'ro' along 'rd'.
// solar: irradiance at the top of the atmosphere. maxDistance in km (-1 = to ground/top).
ScatteringResult IntegrateScattering(vec3 ro, vec3 rd, vec3 sunDir, vec3 solar, int steps, float maxDistance,
                                     bool includeGround, bool useMultiScattering, bool isotropicPhase) {
    ScatteringResult r;
    r.luminance = vec3(0.0);
    r.transmittance = vec3(1.0);
    r.multiScatAs1 = vec3(0.0);

    float tBottom = RaySphere(ro, rd, BottomRadius());
    float tTop = RaySphere(ro, rd, TopRadius());
    float tMax;
    if (tBottom < 0.0) {
        if (tTop < 0.0) return r; // ray misses the atmosphere
        tMax = tTop;
    } else {
        tMax = tTop > 0.0 ? min(tTop, tBottom) : tBottom;
    }
    if (maxDistance > 0.0) tMax = min(tMax, maxDistance);

    float cosTheta = dot(sunDir, rd);
    float phaseR = isotropicPhase ? 1.0 / (4.0 * PI) : RayleighPhase(cosTheta);
    float phaseM = isotropicPhase ? 1.0 / (4.0 * PI) : MiePhase(cosTheta);

    float dt = tMax / float(steps);
    vec3 throughput = vec3(1.0);
    for (int i = 0; i < steps; ++i) {
        float t = (float(i) + 0.3) * dt;
        vec3 p = ro + rd * t;
        float h = length(p);
        vec3 up = p / h;
        Medium m = SampleMedium(p);
        vec3 sampleTransmittance = exp(-m.extinction * dt);

        float sunZenithCos = dot(sunDir, up);
        vec3 transmittanceToSun = SampleTransmittance(h, sunZenithCos);
        float tEarth = RaySphere(p, sunDir, BottomRadius());
        float earthShadow = tEarth >= 0.0 ? 0.0 : 1.0;
        // Multiple scattering fades as the sun sinks below the local horizon (Earth's shadow),
        // otherwise the still-sunlit upper atmosphere would glow orange deep into the night.
        float msShadow = clamp((sunZenithCos + 0.25) / 0.25, 0.0, 1.0);
        vec3 multiScat = useMultiScattering ? SampleMultiScattering(h, sunZenithCos) * (msShadow * msShadow) : vec3(0.0);

        vec3 phaseScattering = m.rayleighScattering * phaseR + m.mieScattering * phaseM;
        vec3 S = solar * (earthShadow * transmittanceToSun * phaseScattering + multiScat * m.scattering);

        // Energy-conserving analytic integration over the step (Hillaire 2015).
        vec3 safeExt = max(m.extinction, vec3(1e-7));
        vec3 sInt = (S - S * sampleTransmittance) / safeExt;
        r.luminance += throughput * sInt;
        vec3 msInt = (m.scattering - m.scattering * sampleTransmittance) / safeExt;
        r.multiScatAs1 += throughput * msInt;
        throughput *= sampleTransmittance;
    }

    if (includeGround && tBottom >= 0.0 && (maxDistance < 0.0 || tBottom <= maxDistance)) {
        vec3 p = ro + rd * tBottom;
        vec3 up = normalize(p);
        float NdotL = clamp(dot(up, sunDir), 0.0, 1.0);
        vec3 transmittanceToSun = SampleTransmittance(length(p), dot(sunDir, up));
        r.luminance += solar * transmittanceToSun * throughput * NdotL * ubo.ozone.w / PI;
    }
    r.transmittance = throughput;
    return r;
}

// Sun irradiance after the atmosphere at a given planet-space position (0 below the horizon).
vec3 SunIrradianceAt(vec3 atmoPos) {
    float h = length(atmoPos);
    vec3 up = atmoPos / h;
    float tEarth = RaySphere(atmoPos, ubo.sunDir.xyz, BottomRadius());
    if (tEarth >= 0.0) return vec3(0.0);
    return ubo.solarIrradiance.rgb * SampleTransmittance(h, dot(ubo.sunDir.xyz, up));
}

// Sky radiance seen from the camera in direction 'dir' (excludes the sun disc), from the sky-view LUT.
vec3 SkyRadiance(vec3 dir) {
    vec3 camPos = CameraAtmospherePos();
    float viewHeight = length(camPos);
    vec3 up = camPos / viewHeight;
    float viewZenithCos = dot(dir, up);
    vec3 sideSun = normalize(vec3(ubo.sunDir.x, 0.0, ubo.sunDir.z) + vec3(1e-5, 0.0, 0.0));
    vec2 dirXZ = vec2(dir.x, dir.z);
    float lenXZ = length(dirXZ);
    float lightViewCos = lenXZ > 1e-5 ? dot(dirXZ / lenXZ, sideSun.xz) : 1.0;
    bool intersectGround = RaySphere(camPos, dir, BottomRadius()) >= 0.0;
    vec2 uv = SkyViewParamsToUv(intersectGround, viewZenithCos, lightViewCos, viewHeight);
    return texture(lutTex[2], uv).rgb + ubo.night.rgb;
}

// Radiance of the sun disc (with limb darkening) for directions inside the solar cone.
vec3 SunDiscRadiance(vec3 dir) {
    float cosAngle = dot(dir, ubo.sunDir.xyz);
    if (cosAngle < ubo.sunDir.w) return vec3(0.0);
    float solidAngle = 2.0 * PI * (1.0 - ubo.sunDir.w);
    float r = clamp(acos(clamp(cosAngle, -1.0, 1.0)) / max(ubo.solarIrradiance.w, 1e-5), 0.0, 1.0);
    float mu = sqrt(max(1.0 - r * r, 0.0));
    float limb = (1.0 - 0.6 * (1.0 - mu)) / 0.8; // normalised so the disc still integrates to the irradiance
    return SunIrradianceAt(CameraAtmospherePos()) / solidAngle * limb;
}
