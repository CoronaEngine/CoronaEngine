# Principled Stable Planes

`vision/src/render_core/material/principled_bsdf.cpp` 的 `PrincipledLobeSet`
保留原有分层 BSDF，只为纯玻璃和纯金属提供 Stable Plane 接口。

## 进入条件

- 已启用的 sheen、coat 的有效权重都严格为零。
- 纯玻璃：有效 metallic 严格为零，transmission_weight 严格为一。
- 纯金属：有效 metallic 严格为一。
- 所选子 lobe 符合现有 `NearSpec` 粗糙度阈值。

条件按着色点的实际参数求值，支持纹理；关闭的层不参与判断。
不根据 LUT 估计的采样权重判断某一层是否存在。
0.999 透射、0.999 金属、粗糙材质及有效的多层混合保留随机路径。

## 分支与输运

纯玻璃委托 `DielectricLobe` 枚举折射和反射，纯金属委托
`MetallicLobe` 枚举反射；`sample_delta_local` 使用相同的子 lobe。
保留已有 Fresnel、全反射、eta、GGX 遮蔽和 LUT 补偿，并乘入
`WeightedLobe::weight()` 的物理权重，不以采样权重替代它。
方向沿用 MaterialEvaluator 的局部坐标适配，不重复变换子 lobe 坐标。

`StablePlanes::build` 消费枚举，`StablePlanes::fill` 使用匹配的 delta
采样处理未覆盖路径。金属的平面变换仍受已有几何法线一致性条件限制；
玻璃沿用现有非平面折射路径。普通 `evaluate`、`sample` 及其 RNG 消耗保持继承实现。

## 回归验证

`VisionPrincipledStablePlanes` 是实际材质图的 GPU 契约测试；
`VisionReSTIREstimatorTests` 另含实际分层渲染与独立输运参考比较。

```powershell
ctest --test-dir cmake-build-relwithdebinfo -R '^VisionPrincipledStablePlanes$' --output-on-failure
ctest --test-dir cmake-build-relwithdebinfo -R '^VisionReSTIREstimatorTests$' --output-on-failure
```

2026-10-31 验证：CLion 引擎构建通过；材质契约与完整 ReSTIR 回归均通过。
Principled 双层平均能量为 0.10285，独立参考为 0.102084，合并标准误为
0.00110014；递归回退测试也在三个标准误内。StablePlaneData 仍为 240 字节。
Kitchen 在相同相机位置、1920×1080、ReSTIR + Stable Plane、关闭降噪时，
酒杯已恢复透明；截图位于 `.workspace/principled-kitchen-validation/kitchen_restir_sp_1080p.png`。
