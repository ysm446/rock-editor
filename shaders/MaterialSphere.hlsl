// マテリアル 1 つを、回せる球か平面で見るためのプレビュー。
//
// メッシュは使わず、形とレイを解析的に交差させる。**照らし方はビューポートと同じ**
// （適用中の天球の IBL + 太陽 + 露出 + トーンマップ）。素材が本番の環境でどう見えるかを
// そのまま確かめるためで、一覧のサムネイル（MaterialThumbnail.hlsl）とは目的が違う
// （あちらは見比べるための固定 2 灯で、正面から見た円板）。
//
// 変位を付けるときは、先に CsBakeHeight で素材のハイトを形の座標へ焼き、
// CsMain はその高さの面へレイを進めて当たりを探す（ハイトフィールドのレイマーチ）。
// 太陽の影も同じ面で調べるので、凹凸の高さが陰影と影で読める。
// 変位はこのプレビューだけの表示で、ノードの計算やビューポートの形には関係しない。
//
// 背景は環境キューブのぼかしたミップ。ビューポートの背景（Skybox）と同じ絵を、
// 同じ露出とトーンマップで出す。

#include "Brdf.hlsli"
#include "CompositeCommon.hlsli"
#include "LayerMaterial.hlsli"
#include "EnvCommon.hlsli"
#include "Tonemap.hlsli"

struct SphereConstants
{
    uint outputIndex;
    uint size;               // 出力は正方形
    uint baseColorIndex;     // sRGB の SRV。kInvalidTextureIndex なら定数
    uint normalIndex;

    uint roughnessIndex;
    uint metallicIndex;
    uint aoIndex;
    uint mapChannels;        // 4bit ずつ ROCK_CHANNEL_SLOT_* の順

    float3 baseColorTint;
    float roughnessValue;

    float metallicValue;
    float aoValue;
    float uvScale;           // 形の 1 周（平面は一辺）に並べるタイル数
    uint flipNormalGreen;    // 0 以外なら法線マップの緑を反転して読む

    float3 cameraPosition;   // 形の中心は原点。球は半径 1、平面は一辺 2
    float tanHalfFov;

    float3 lightDirection;   // サーフェスから光源へ向かう方向
    float lightIlluminance;

    float3 lightColor;
    float iblIntensity;

    uint irradianceIndex;    // kInvalidTextureIndex なら IBL を掛けない
    uint prefilteredIndex;
    uint brdfLutIndex;
    uint environmentIndex;   // 背景。kInvalidTextureIndex なら無地

    uint prefilteredMipCount;
    float backgroundMip;
    float exposure;
    uint tonemapMode;

    // ベースカラーの調整（ティントを掛けたあとに効く）。合成と同じ値を渡すこと。
    float2 colorAdjust;  // 色相（ラジアン）, 彩度
    float brightness;
    float pad0;

    uint shape;              // 0 球、1 平面
    // 変位の幅（ハイト 0〜1 の差）。球の半径・平面の一辺の半分を 1 とした長さ。0 なら変位しない
    float displacement;
    uint heightFieldIndex;   // 焼いた高さの SRV（CsMain が読む）
    uint heightFieldUav;     // 焼いた高さの UAV（CsBakeHeight が書く）

    uint heightIndex;        // 素材のハイトマップ。kInvalidTextureIndex なら平ら
    uint heightFieldSize;
    float2 pad1;
    LayerMaterialData layerMaterial;
};

ConstantBuffer<SphereConstants> g_sphere : register(b1);

// 背景が無いときの色（リニア）。露出を掛ける前の値なので、
// 明るさは天球を出しているときと同じくらいに見える程度で足りる。
static const float3 kFallbackBackground = float3(0.02f, 0.022f, 0.026f);

// 平面の変位で、下に付ける厚み（変位の幅に足す分）。平面を斜めから見たとき、
// 凹凸の高さが側面の段として読めるようにする。
static const float kPlaneBaseThickness = 0.04f;
// 高さの面を探すレイの刻み数と、当たった区間を詰める回数。
static const uint kMarchSteps = 128;
static const uint kRefineSteps = 8;
static const uint kShadowSteps = 64;

// UV の差分。u は 1 周で巻き戻るので、継ぎ目をまたぐ差は短いほうへ畳む。
// 畳まないと、継ぎ目の 1 列だけミップが最下段まで落ちて帯に見える。
float2 WrapDelta(float2 delta)
{
    return delta - round(float2(delta.x, 0.0f));
}

// 画素の大きさに見合ったミップを選ぶ。コンピュートには微分が無いので、
// 隣の画素との UV 差から自分で求める（ハードウェアの選び方と同じ式）。
float MapLod(uint index, float2 deltaX, float2 deltaY)
{
    Texture2D<float4> map = ResourceDescriptorHeap[index];
    float2 dimensions;
    float levels;
    map.GetDimensions(0, dimensions.x, dimensions.y, levels);

    const float2 dx = deltaX * dimensions;
    const float2 dy = deltaY * dimensions;
    return 0.5f * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8f));
}

float4 SampleMap(uint index, float2 uv, float lod)
{
    Texture2D<float4> map = ResourceDescriptorHeap[index];
    return map.SampleLevel(g_samplerLinearWrap, uv, lod);
}

float SampleScalarMap(uint index, uint channelSlot, float2 uv, float lod)
{
    return SelectChannel(SampleMap(index, uv, lod),
                         UnpackChannel(g_sphere.mapChannels, channelSlot));
}

// 画素の中心から出るレイ。y は下向きの画素座標なので、上向きの基底に対して反転する。
float3 RayDirection(float2 pixel, float3 forward, float3 right, float3 up)
{
    const float2 ndc = ((pixel + 0.5f) / float(g_sphere.size)) * 2.0f - 1.0f;
    return normalize(forward + right * (ndc.x * g_sphere.tanHalfFov) -
                     up * (ndc.y * g_sphere.tanHalfFov));
}

// 単位球の法線。**外れたレイでも最も近い点の向きを返す。**
// 輪郭のぼかし（被覆）に使う画素は球を外れているので、そこで法線が無いと
// 縁の色が決まらない。外れていても連続した向きが取れるようにしておく。
float3 SphereNormal(float3 origin, float3 direction)
{
    const float b = dot(origin, direction);
    const float c = dot(origin, origin) - 1.0f;
    const float discriminant = b * b - c;
    const float t = -b - sqrt(max(discriminant, 0.0f));
    return normalize(origin + direction * t);
}

// 球の中心から、レイまでの最短距離。1 が輪郭。
float SilhouetteDistance(float3 origin, float3 direction)
{
    const float b = dot(origin, direction);
    return sqrt(max(dot(origin, origin) - b * b, 0.0f));
}

// 変位の無い形との交点。ミップを選ぶための UV に使う。
// 平面は y = 0。レイが平面と平行なら原点側の十分遠い点を返す。
float3 BasePoint(float3 origin, float3 direction)
{
    if (g_sphere.shape == 0)
    {
        return SphereNormal(origin, direction);
    }
    const float t = abs(direction.y) > 1e-5f ? -origin.y / direction.y : 1e5f;
    return origin + direction * max(t, 0.0f);
}

// 形の座標（0〜1）。球は緯度経度、平面は x が u、-z が v（v は奥へ増える）。
float2 ObjectUv(float3 p)
{
    if (g_sphere.shape == 0)
    {
        return DirectionToEquirectUv(normalize(p));
    }
    return float2(p.x * 0.5f + 0.5f, 0.5f - p.z * 0.5f);
}

// 焼いた高さから求めた、変位の無い形からのずれ。
float Displacement(float2 objectUv)
{
    Texture2D<float> field = ResourceDescriptorHeap[g_sphere.heightFieldIndex];
    const float h = g_sphere.shape == 0 ? field.SampleLevel(g_samplerLinearWrap, objectUv, 0.0f)
                                        : field.SampleLevel(g_samplerLinearClamp, objectUv, 0.0f);
    return (h - 0.5f) * g_sphere.displacement;
}

// 変位した形の内外。**負が中。** 距離としては正確でないので、符号と勾配だけに使う。
float Inside(float3 p)
{
    if (g_sphere.shape == 0)
    {
        return length(p) - 1.0f - Displacement(ObjectUv(p));
    }
    // 平面は一辺 2 の板。上面が変位した面、下面は変位の最も低い所よりさらに下。
    const float bottom = 0.5f * g_sphere.displacement + kPlaneBaseThickness;
    const float top = p.y - Displacement(ObjectUv(p));
    return max(max(top, -p.y - bottom), max(abs(p.x), abs(p.z)) - 1.0f);
}

// 変位した形の法線。内外の値の勾配から求める。
float3 DisplacedNormal(float3 p)
{
    const float e = 2.0f / float(max(g_sphere.heightFieldSize, 1u));
    const float3 gradient = float3(
        Inside(p + float3(e, 0, 0)) - Inside(p - float3(e, 0, 0)),
        Inside(p + float3(0, e, 0)) - Inside(p - float3(0, e, 0)),
        Inside(p + float3(0, 0, e)) - Inside(p - float3(0, 0, e)));
    return normalize(gradient + 1e-8f);
}

// 変位した形を囲む範囲とレイの区間。囲みに入らなければ false。
bool Bounds(float3 origin, float3 direction, out float t0, out float t1)
{
    t0 = 0.0f;
    t1 = 0.0f;
    if (g_sphere.shape == 0)
    {
        const float outer = 1.0f + 0.5f * g_sphere.displacement + 1e-3f;
        const float b = dot(origin, direction);
        const float c = dot(origin, origin) - outer * outer;
        const float discriminant = b * b - c;
        if (discriminant < 0.0f)
        {
            return false;
        }
        const float root = sqrt(discriminant);
        t0 = max(-b - root, 0.0f);
        t1 = -b + root;
        // 変位の最も低い所より内側の球に当たれば、そこまでに必ず表面がある。
        const float inner = 1.0f - 0.5f * g_sphere.displacement;
        const float innerC = dot(origin, origin) - inner * inner;
        const float innerDisc = b * b - innerC;
        // 影のレイのように表面から外へ出るときは、内側の球の交点が後ろにあるので使わない。
        const float innerT = -b - sqrt(max(innerDisc, 0.0f));
        if (inner > 0.0f && innerDisc >= 0.0f && innerT > t0)
        {
            t1 = min(t1, innerT + 1e-3f);
        }
        return t1 > t0;
    }
    const float bottom = 0.5f * g_sphere.displacement + kPlaneBaseThickness;
    const float3 lo = float3(-1.0f, -bottom, -1.0f) - 1e-3f;
    const float3 hi = float3(1.0f, 0.5f * g_sphere.displacement, 1.0f) + 1e-3f;
    // 軸と平行なレイで 0 除算にならないよう、極小値で置き換えてから逆数を取る。
    const float3 inv = 1.0f / select(abs(direction) > 1e-8f, direction, 1e-8f);
    const float3 a = (lo - origin) * inv;
    const float3 b = (hi - origin) * inv;
    const float3 near = min(a, b);
    const float3 far = max(a, b);
    t0 = max(max(max(near.x, near.y), near.z), 0.0f);
    t1 = min(min(far.x, far.y), far.z);
    return t1 > t0;
}

// レイを刻んで変位した形との最初の交点を探す。見つかれば t を返す。
bool MarchSurface(float3 origin, float3 direction, out float tHit)
{
    tHit = 0.0f;
    float t0, t1;
    if (!Bounds(origin, direction, t0, t1))
    {
        return false;
    }
    const float step = (t1 - t0) / float(kMarchSteps);
    float previous = t0;
    if (Inside(origin + direction * t0) < 0.0f)
    {
        tHit = t0;  // 囲みに入った所がもう中（平面の側面など）。
        return true;
    }
    [loop] for (uint i = 1; i <= kMarchSteps; ++i)
    {
        const float t = t0 + step * float(i);
        if (Inside(origin + direction * t) < 0.0f)
        {
            // 外だった previous と中だった t の間を二分で詰める。
            float lo = previous, hi = t;
            [unroll] for (uint k = 0; k < kRefineSteps; ++k)
            {
                const float mid = 0.5f * (lo + hi);
                if (Inside(origin + direction * mid) < 0.0f) hi = mid;
                else lo = mid;
            }
            tHit = hi;
            return true;
        }
        previous = t;
    }
    return false;
}

// 太陽へ向かうレイが変位した形に遮られるか。遮られれば 0。
float MarchShadow(float3 position, float3 normal, float3 lightDirection)
{
    const float3 origin = position + normal * (4.0f / float(max(g_sphere.heightFieldSize, 1u)));
    float t0, t1;
    if (!Bounds(origin, lightDirection, t0, t1))
    {
        return 1.0f;
    }
    const float step = (t1 - t0) / float(kShadowSteps);
    [loop] for (uint i = 1; i <= kShadowSteps; ++i)
    {
        if (Inside(origin + lightDirection * (t0 + step * float(i))) < 0.0f)
        {
            return 0.0f;
        }
    }
    return 1.0f;
}

// 素材のハイトを形の座標へ焼く。変位するときだけ、CsMain の前に毎フレーム走らせる。
[numthreads(8, 8, 1)]
void CsBakeHeight(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint size = g_sphere.heightFieldSize;
    if (dispatchThreadId.x >= size || dispatchThreadId.y >= size)
    {
        return;
    }
    RWTexture2D<float> field = ResourceDescriptorHeap[g_sphere.heightFieldUav];
    const float2 objectUv = (float2(dispatchThreadId.xy) + 0.5f) / float(size);
    const float2 uv = objectUv * g_sphere.uvScale;
    const float footprint = g_sphere.uvScale / float(size);
    float height = 0.5f;
    if (g_sphere.layerMaterial.count > 0)
    {
        height = EvaluateLayerMaterialBase(g_sphere.layerMaterial, uv, uv, footprint.xx,
                                           float2(1, 0), float2(0, 1)).height;
    }
    else if (g_sphere.heightIndex != kInvalidTextureIndex)
    {
        Texture2D<float4> map = ResourceDescriptorHeap[g_sphere.heightIndex];
        float2 dimensions;
        float levels;
        map.GetDimensions(0, dimensions.x, dimensions.y, levels);
        const float lod = max(log2(max(footprint * max(dimensions.x, dimensions.y), 1.0f)), 0.0f);
        height = SampleScalarMap(g_sphere.heightIndex, ROCK_CHANNEL_SLOT_HEIGHT, uv, lod);
    }
    field[dispatchThreadId.xy] = height;
}

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_sphere.size || dispatchThreadId.y >= g_sphere.size)
    {
        return;
    }

    RWTexture2D<float4> output = ResourceDescriptorHeap[g_sphere.outputIndex];

    // --- カメラ ------------------------------------------------------------
    // 原点を見る軌道カメラ。**仰角は C++ 側で ±85 度に制限してある**ので、
    // ここで上方向との縮退（外積が 0 になる）を気にしなくてよい。
    const float3 origin = g_sphere.cameraPosition;
    const float3 forward = normalize(-origin);
    const float3 right = normalize(cross(forward, float3(0.0f, 1.0f, 0.0f)));
    const float3 up = cross(right, forward);

    const float2 pixel = float2(dispatchThreadId.xy);
    const float3 direction = RayDirection(pixel, forward, right, up);

    // --- 背景 --------------------------------------------------------------
    float3 background = kFallbackBackground;
    if (g_sphere.environmentIndex != kInvalidTextureIndex)
    {
        TextureCube<float4> environment = ResourceDescriptorHeap[g_sphere.environmentIndex];
        background =
            environment.SampleLevel(g_samplerLinearClamp, direction, g_sphere.backgroundMip).rgb *
            g_sphere.iblIntensity;
    }

    // --- 形との交点と輪郭の被覆 ---------------------------------------------
    // 画素の角幅 × 距離が、形の表面での画素の大きさにあたる。
    const float pixelWidth = (2.0f * g_sphere.tanHalfFov / float(g_sphere.size)) * length(origin);
    const bool displaced = g_sphere.displacement > 0.0f;
    float coverage = 0.0f;
    float3 position = 0.0f;
    float3 normalGeometric = float3(0.0f, 1.0f, 0.0f);
    if (displaced)
    {
        // 変位した面は解析的に解けないので、当たり外れの 0/1 で抜く。
        float t;
        if (MarchSurface(origin, direction, t))
        {
            coverage = 1.0f;
            position = origin + direction * t;
            normalGeometric = DisplacedNormal(position);
        }
    }
    else if (g_sphere.shape == 0)
    {
        // 球を解析的に持っているので、判定を 0/1 にせず輪郭をまたぐ幅で滑らかにする。
        coverage = 1.0f - smoothstep(1.0f - pixelWidth, 1.0f + pixelWidth,
                                     SilhouetteDistance(origin, direction));
        normalGeometric = SphereNormal(origin, direction);
        position = normalGeometric;  // 半径 1 なので法線と同じ
    }
    else if (abs(direction.y) > 1e-5f && origin.y * direction.y < 0.0f)
    {
        // 平面は y = 0 の一辺 2 の正方形。縁は画素の大きさでぼかす。
        position = origin + direction * (-origin.y / direction.y);
        const float edge = 1.0f - max(abs(position.x), abs(position.z));
        coverage = saturate(edge / pixelWidth + 0.5f);
        // 裏から見たときは裏面を向ける。
        normalGeometric = float3(0.0f, origin.y > 0.0f ? 1.0f : -1.0f, 0.0f);
    }
    if (coverage <= 0.0f)
    {
        output[dispatchThreadId.xy] =
            float4(LinearToSrgb(ApplyTonemap(background * g_sphere.exposure,
                                             g_sphere.tonemapMode)),
                   1.0f);
        return;
    }
    const float3 viewDirection = normalize(origin - position);

    // マップは形の座標で貼る。uvScale で並べる数を決める。
    const float2 uv = ObjectUv(position) * g_sphere.uvScale;
    // 隣の画素の UV。ミップを選ぶためだけに使うので、変位の無い形で求める。
    const float2 uvBase = ObjectUv(BasePoint(origin, direction)) * g_sphere.uvScale;
    const float2 uvX = ObjectUv(BasePoint(origin, RayDirection(pixel + float2(1.0f, 0.0f), forward, right, up))) *
                       g_sphere.uvScale;
    const float2 uvY = ObjectUv(BasePoint(origin, RayDirection(pixel + float2(0.0f, 1.0f), forward, right, up))) *
                       g_sphere.uvScale;
    const float2 deltaX = WrapDelta(uvX - uvBase);
    const float2 deltaY = WrapDelta(uvY - uvBase);

    float3 baseColor = g_sphere.baseColorTint;
    if (g_sphere.baseColorIndex != kInvalidTextureIndex)
    {
        baseColor *= SampleMap(g_sphere.baseColorIndex, uv,
                               MapLod(g_sphere.baseColorIndex, deltaX, deltaY))
                         .rgb;
    }
    baseColor = AdjustBaseColor(baseColor, g_sphere.colorAdjust.x, g_sphere.colorAdjust.y, g_sphere.brightness);

    float roughness = g_sphere.roughnessValue;
    if (g_sphere.roughnessIndex != kInvalidTextureIndex)
    {
        roughness = SampleScalarMap(g_sphere.roughnessIndex, ROCK_CHANNEL_SLOT_ROUGHNESS, uv,
                                    MapLod(g_sphere.roughnessIndex, deltaX, deltaY));
    }

    float metallic = g_sphere.metallicValue;
    if (g_sphere.metallicIndex != kInvalidTextureIndex)
    {
        metallic = SampleScalarMap(g_sphere.metallicIndex, ROCK_CHANNEL_SLOT_METALLIC, uv,
                                   MapLod(g_sphere.metallicIndex, deltaX, deltaY));
    }

    float ambientOcclusion = g_sphere.aoValue;
    if (g_sphere.aoIndex != kInvalidTextureIndex)
    {
        ambientOcclusion = SampleScalarMap(g_sphere.aoIndex, ROCK_CHANNEL_SLOT_AO, uv,
                                           MapLod(g_sphere.aoIndex, deltaX, deltaY));
    }

    // --- 法線 --------------------------------------------------------------
    // 接空間は形の座標の貼り方から作る。u が増える向きが接線、v が増える向きが従法線。
    // 球は緯度経度（v は北極から南へ）、平面は u = +X、v = -Z。
    // 変位した面では、その面の法線に対して接線を立て直す。
    // **このアプリの接空間は DirectX 規約**なので、緑 = +V として読む。
    // OpenGL 規約のマップは緑を反転する。
    float3 tangentGuide = float3(1.0f, 0.0f, 0.0f);
    bool hasTangent = true;
    if (g_sphere.shape == 0)
    {
        const float3 around = normalize(position);
        // 極では接線が縮退するので、そのときは幾何法線のまま使う。
        hasTangent = length(around.xz) > 1e-3f;
        tangentGuide = hasTangent ? normalize(float3(-around.z, 0.0f, around.x)) : tangentGuide;
    }
    else if (abs(dot(normalGeometric, tangentGuide)) > 0.99f)
    {
        tangentGuide = float3(0.0f, 0.0f, -1.0f);  // 平面の ±X 側面
    }
    const float3 tangent = normalize(tangentGuide - normalGeometric * dot(normalGeometric, tangentGuide));
    // 赤道・経度 0 では T=(0,0,1)、N=(1,0,0)、B=N×T=(0,-1,0)（v が下へ増える向き）。
    const float3 bitangent = cross(normalGeometric, tangent);

    float3 normal = normalGeometric;
    if (g_sphere.normalIndex != kInvalidTextureIndex && hasTangent)
    {
        float3 sampled = SampleMap(g_sphere.normalIndex, uv,
                                   MapLod(g_sphere.normalIndex, deltaX, deltaY))
                                 .rgb *
                             2.0f - 1.0f;
        if (g_sphere.flipNormalGreen != 0u)
        {
            sampled.y = -sampled.y;
        }
        normal = normalize(tangent * sampled.x + bitangent * sampled.y + normalGeometric * sampled.z);
    }

    if (g_sphere.layerMaterial.count > 0) {
        const float footprint = max(length(deltaX), length(deltaY));
        LayerMaterialSample mixed = EvaluateLayerMaterial(g_sphere.layerMaterial, uv, uv, footprint.xx,
            float2(1,1), float2(1,0), float2(0,1));
        baseColor = mixed.color; roughness = mixed.surface.x; metallic = mixed.surface.y; ambientOcclusion = mixed.surface.z;
        if (hasTangent) {
            normal = normalize(tangent * mixed.normal.x + bitangent * mixed.normal.y + normalGeometric * mixed.normal.z);
        }
    }

    // --- 陰影（ビューポートと同じ式）---------------------------------------
    float3 diffuseColor;
    float3 f0;
    SplitBaseColor(baseColor, metallic, diffuseColor, f0);
    const float clampedRoughness = clamp(roughness, kMinPerceptualRoughness, 1.0f);

    const float3 lightDirection = normalize(g_sphere.lightDirection);
    // 変位したときだけ、凹凸が落とす太陽の影を付ける。
    const float shadow = displaced ? MarchShadow(position, normalGeometric, lightDirection) : 1.0f;
    float3 radiance = ShadeDirectionalLight(normal, viewDirection, lightDirection,
                                            g_sphere.lightColor, g_sphere.lightIlluminance * shadow,
                                            diffuseColor, f0, clampedRoughness);

    if (g_sphere.irradianceIndex != kInvalidTextureIndex)
    {
        // MeshPbr と同じ分割和近似。nDotV は 1 を超えると NaN になるので clamp で守る。
        const float nDotV = clamp(dot(normal, viewDirection), 1e-4f, 1.0f);

        TextureCube<float4> irradianceMap = ResourceDescriptorHeap[g_sphere.irradianceIndex];
        TextureCube<float4> prefilteredMap = ResourceDescriptorHeap[g_sphere.prefilteredIndex];
        Texture2D<float2> brdfLut = ResourceDescriptorHeap[g_sphere.brdfLutIndex];

        const float3 irradiance =
            irradianceMap.SampleLevel(g_samplerLinearClamp, normal, 0.0f).rgb;
        const float3 fresnel = FresnelSchlickRoughness(f0, nDotV, clampedRoughness);
        const float3 diffuseIbl = (1.0f - fresnel) * diffuseColor * irradiance;

        const float3 reflectionDirection = reflect(-viewDirection, normal);
        const float mipLevel =
            clampedRoughness * float(max(g_sphere.prefilteredMipCount, 1u) - 1u);
        const float3 prefiltered =
            prefilteredMap.SampleLevel(g_samplerLinearClamp, reflectionDirection, mipLevel).rgb;
        const float2 environmentBrdf =
            brdfLut.SampleLevel(g_samplerLinearClamp, float2(nDotV, clampedRoughness), 0.0f);
        const float3 specularIbl = prefiltered * (f0 * environmentBrdf.x + environmentBrdf.y);

        radiance += (diffuseIbl + specularIbl) * g_sphere.iblIntensity * ambientOcclusion;
    }

    const float3 color = lerp(background, radiance, coverage) * g_sphere.exposure;
    output[dispatchThreadId.xy] =
        float4(LinearToSrgb(ApplyTonemap(color, g_sphere.tonemapMode)), 1.0f);
}
