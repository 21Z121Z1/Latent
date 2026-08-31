# Latent architecture

## 1. Architectural thesis

Latent is a **semantic imaging compiler/runtime**, not a Vulkan library and not a bag of ISP stages.

The stable architectural order is:

> **scene/sensor/display semantics first, graph compilation second, execution backend third, evidence throughout.**

That order is intentionally asymmetric. A Vulkan resource, shader pass, codec API, or Android handle may realize a semantic operation, but none of them may define what the image means.

The system is easiest to understand as two coupled towers:

1. a **semantic tower** that moves image meaning from sensor measurements to a scene master, then to explicit display renditions and finally encoded artifacts; and
2. a **realization tower** that turns semantic operations into reference behavior, compiled execution plans, production lowerings, and verification evidence.

These towers intersect at every operation. That intersection is the core abstraction of Latent.

## 2. Semantic tower: image meaning changes only at explicit boundaries

```text
L4  Encoded artifact / codec container
        ^
        | explicit codec/export boundary
        |
L3  RenderedFrame
    display-referred, encoded, target-specific
        ^
        | explicit rendering boundary
        |
L2  SceneFrame
    scene-referred, linear AP1/D60, unbounded, negative-preserving
        ^
        | reconstruction / scene-estimation boundary
        |
L1  RawFrame / future RawBurst
    sensor-referred samples + capture metadata + provenance
        ^
        | capture / ingress boundary
        |
L0  Camera2 / NDK Camera / recorded fixtures / external input
```

The most important property is not the names of these structs; it is that each level has a different meaning and therefore a different set of legal operations.

| Semantic state | Canonical current type | What is legal | What must not be hidden here |
| --- | --- | --- | --- |
| Sensor-referred | `RawFrame` | packing/stride interpretation, black/white selection, sensor normalization, defect/LSC/WB, noise/provenance handling | display tone, display transfer, final picture brightness |
| Scene-referred | `SceneFrame` | scene analysis, scene-linear reconstruction/denoise/merge, scene coordinate transforms | display clipping, sRGB/PQ/HLG encoding, codec gain-map policy |
| Display-referred | `RenderedFrame` | render exposure, tone/gamut decisions, output primaries, target white/peak, display transfer | rewriting capture radiometry or scene master coordinates |
| Codec/export | `UltraHdrRenditionPair`, encoded bytes | packing, standards metadata, container behavior, external codec integration | redefining scene or rendering semantics |

### 2.1 `SceneFrame` is the architectural center

`SceneFrame` is the master computational image object. Current semantics are:

- linear ACEScg/AP1 primaries with D60 white;
- scene-referred;
- unbounded;
- negative coordinates are legal;
- `sceneScaleEV` is an explicit coordinate scale;
- propagated noise and confidence/provenance data may accompany the scene image.

`sceneScaleEV` is **not** display white, middle gray, final image brightness, capture exposure, or tone-map exposure. Rendering must remove this representational coordinate choice before applying `renderExposureEV`.

This separation is already tested by the output/reference branches and must remain a first-class invariant.

### 2.2 Domain changes are operations, not conventions

A transition such as sensor -> scene or scene -> display must be visible in the graph/type system. The current `ImageType` and `ImagingGraph` already validate some of these conditions: scene values must be linear/unbounded/negative-permitting, and operations that change `ReferenceDomain` must declare that change.

Target architecture should make illegal combinations harder to construct rather than relying on every caller to fill a free-form struct correctly. Canonical semantic constructors/schemas are preferred over repeated field combinations.

## 3. Realization tower: what, how, and proof are separate

```text
semantic types + operation schemas
              |
              v
        SemanticGraph
              |
       validation / normalization
              |
              v
        GraphCompiler
              |
      capability + policy aware lowering
              |
              v
        ExecutionPlan
              |
        +-----+-------------------+
        |                         |
        v                         v
reference executor          production executor
FP32 deterministic          Vulkan / platform / external
        |                         |
        +-----------+-------------+
                    |
                    v
             evidence + trace
```

`SemanticGraph`, `GraphCompiler`, and `ExecutionPlan` are architectural names here. The repository currently has an `ImagingGraph` seed and direct reference/Vulkan entry points, but it does **not** yet contain a complete compiler or first-class execution plan.

### 3.1 Semantic contracts

Semantic types own image meaning. Operation schemas should eventually own intrinsic operation facts such as:

- input/output domains and type constraints;
- point/neighborhood/reduction/temporal/external access pattern;
- purity;
- whether fusion is semantically legal;
- precision requirements;
- whether an operation changes reference domain;
- required metadata/provenance;
- reference implementation availability;
- supported production lowerings.

Today `OperationDescriptor` allows callers to provide several of these traits manually. That was appropriate for the first IR slice but is too permissive for a system an agent must reason about reliably. A caller should not be able to label the same `OperationKind` as both pure and impure, or as domain-changing in one graph and not another, without an explicit specialized schema.

### 3.2 Reference semantics are executable specification

The FP32 reference path is not a slow version of Vulkan. It is the semantic oracle against which production implementations are compared.

Reference code should optimize for:

- determinism;
- explicit intermediate meaning;
- auditable standards/reference provenance;
- reproducible precision policy;
- golden/property-test friendliness;
- semantic equivalence with production operations.

Production code may fuse operations, change storage precision, alias memory, or select device-specific paths only when the resulting error remains inside an explicit validation budget.

### 3.3 Graph compiler: the missing architectural hinge

The graph compiler should become the single control plane that answers:

- Which semantic operations are requested?
- Are their types/domains/provenance legal?
- Which operations may be fused without changing meaning?
- Which precision/storage boundaries are permitted?
- Which backend/lowering is selected for this device and request?
- Where are reductions, temporal barriers, external interfaces, and reference-domain transitions?
- What are resource lifetimes and synchronization requirements?
- Why was a particular fallback or fast path selected?

This is the highest-leverage missing abstraction. Without it, `ImagingGraph`, `ImagingBackend`, kernel wrappers, and direct reference functions risk becoming competing orchestration surfaces.

### 3.4 Execution plan: explicit mechanism, no semantic invention

A future `ExecutionPlan` should be a backend-ready but semantics-linked artifact. It should contain enough information to execute and diagnose a frame without reopening the entire source tree:

- stable operation/resource IDs;
- selected lowering for each semantic operation/group;
- resource types, formats, extents, ownership, and lifetimes;
- precision/storage decisions;
- dispatch dependencies/barriers;
- external ingress/egress boundaries;
- required/selected capabilities;
- semantic operation references for traceability;
- validation/tolerance metadata where relevant.

It should not contain creative image-policy decisions that were absent from the semantic request.

### 3.5 Executors

Executors perform an already-decided plan.

- The deterministic reference executor/path establishes semantic behavior.
- Vulkan is the primary production compute realization.
- Platform capture/display adapters and external codecs are integration executors/adapters at system edges.

`ComputeRunner` is currently a deliberately synchronous one-queue harness with host-visible buffers and one-pipeline dispatches. It is a correctness seed, not the final scheduler, allocator, or multi-frame executor.

## 4. Evidence plane: correctness is part of the architecture

Every significant semantic operation should have an evidence chain:

```text
standard / paper / explicit project invariant
                |
                v
       deterministic reference
                |
      golden + property tests
                |
                v
       production lowering
                |
      differential / integration tests
                |
                v
        CI + execution trace
```

Evidence is not an after-the-fact QA layer. It is how Latent permits aggressive fusion, FP16 storage, Vulkan execution, and external integration without losing semantic control.

For exact commands and the current test matrix, see `docs/verification.md`.

## 5. Current implementation mapped onto the architecture

### 5.1 Sensor model and single-RAW reference reconstruction

`RawFrame` carries canonical raw storage plus CFA, exposure information, black/white candidates, neutral/color gains, lens shading, noise profile, defects, and metadata source/validity/confidence.

Current single-RAW reference flow:

```text
RawFrame
  -> metadata validation
  -> optical/dynamic/static black selection
  -> dynamic/static white selection
  -> sensor normalization (FP32, no clamp)
  -> defect correction / lens shading / white balance
  -> Malvar-He-Cutler or baseline demosaic
  -> DNG camera color model -> XYZ D50 -> ACEScg/AP1 D60
  -> explicit 2^sceneScaleEV coordinate scale
  -> SceneFrame
```

The deterministic box-average demosaic remains available as a comparison/reference baseline; Malvar-He-Cutler 2004 is the normal reference reconstruction choice.

### 5.2 Color science

`imaging/ColorScience` and `reference/DngColor` implement the current camera-to-scene color model:

- SMPTE RP 177 primary/XYZ derivation;
- Bradford chromatic adaptation;
- Robertson (1968) correlated-color temperature in the Adobe DNG SDK style;
- DNG Chapter 6 dual-illuminant interpolation in mired space;
- camera-neutral <-> white-balance xy conversion;
- preferred ForwardMatrix path plus inverse-color-matrix fallback;
- XYZ D50 -> linear AP1/D60.

Reference/golden validation is intentionally tied to published/official implementations rather than to production shader output.

### 5.3 Sensor correction and uncertainty

`reference/SensorLinearOps`, `imaging/Noise`, and `reference/NoisePropagation` provide:

- map-driven defect correction plus conservative noise-aware detection;
- Android-convention 4-channel lens-shading map interpolation;
- NOISE_PROFILE transformation into sensor-linear coordinates;
- variance composition through LSC, WB, demosaic taps, color transform, and scene scale;
- lazy per-pixel/channel propagated sigma queries on `SceneFrame`.

Monte Carlo validation is used because uncertainty propagation is itself a semantic contract.

### 5.4 Vulkan ingress and capabilities

`imaging/RawPacking`, `vulkan/IngressPlan`, and `vulkan/VulkanRuntime` separate portable correctness from optional fast paths.

Portable baseline:

```text
AImage/recorded planes
  -> respect packing + stride
  -> RAW10/RAW12 unpack/copy
  -> canonical uint16 sensor storage
```

Potential fast path:

```text
AImage -> AHardwareBuffer -> capability/format probe -> external-memory import
```

Direct import is never a correctness prerequisite. Claims must distinguish:

1. shared allocation/handle availability;
2. Vulkan import success;
3. measured memory-traffic elimination.

`DeviceCaps` records the Vulkan 1.1 baseline and optional acceleration capabilities such as FP16 storage/arithmetic, timeline semaphores, synchronization2, subgroup support, descriptor indexing, float controls, integer dot product, cooperative matrix, and AHardwareBuffer external memory.

### 5.5 Production Vulkan K1a/K1b

The first production lowering chain is deliberately narrow and differentially verified.

K1a (`sensor_preprocess.comp` + `SensorPreprocessKernel`):

```text
canonical uint16 RAW
 -> black subtraction
 -> white normalization
 -> lens shading
 -> white balance
 -> packed FP16 Bayer storage
```

Computation is FP32; FP16 is a validated storage boundary.

K1b (`demosaic_color.comp` + `DemosaicColorKernel`):

```text
FP16 Bayer
 -> deterministic box or MHC demosaic (FP32 accumulation)
 -> camera-to-scene color transform (FP32)
 -> explicit scene scale
 -> packed RGBA16F scene storage
```

Tests cover CFA patterns, odd/extreme extents, LSC on/off, both demosaic methods, negative/sign preservation, NaN absence, and error budgets after FP16 boundaries.

### 5.6 Scene analysis and independent rendering

`render/SceneAnalysis`, `render/AcesToneScale`, `render/OutputEncoding`, and `render/ReferenceRenderer` establish the scene -> display boundary.

Key rules:

- analysis removes `sceneScaleEV` before deriving exposure statistics;
- rendering then applies an independent `renderExposureEV`;
- SDR and HDR are independent branches from the same scene master;
- SDR target is Rec.709/D65/sRGB;
- HDR target is BT.2020/D65/PQ;
- the ACES-derived scalar tonescale is used only as a luminance primitive;
- the current deterministic neutral-axis gamut compression is explicit and is **not** claimed to be the complete ACES 2 Output Transform;
- `RenderedFrame` is display-referred and keeps target peak, nominal white, and HDR headroom alongside encoded pixels.

Production Vulkan rendering is not implemented yet.

### 5.7 Ultra HDR codec boundary

`codec/UltraHdrStaging` converts two explicit display renditions into the packed input forms expected by libultrahdr:

```text
SceneFrame
  |-- SDR render -> Rec.709/sRGB RenderedFrame -> RGBA8888 --|
  |-- HDR render -> BT.2020/PQ RenderedFrame -> RGBA1010102 -|
                                                              v
                                                    libultrahdr
                                               gain map + metadata +
                                                     container
```

This is an important ownership boundary: gain-map math relates explicit SDR/HDR renditions and belongs to the codec integration layer, not to RAW reconstruction or the scene master.

The external libultrahdr dependency is isolated in `latent::codec` so its build/fast-FP policies do not alter the deterministic reference core. JPEG integration is exercised in CI; HEIF/AVIF routing exists but still needs full platform/dependency validation.

## 6. Precision policy

Precision is semantic where it can change results and implementation detail where it cannot.

- Reference reconstruction/rendering: FP32 deterministic behavior.
- RAW storage: packed/native integer until canonical ingress conversion.
- Current production scene storage target: RGBA16F where validated.
- Demosaic/color cancellation-sensitive math: FP32 accumulation.
- Future burst accumulation, motion/statistics, reductions, and similar numerically sensitive work: FP32 unless separate evidence justifies another choice.
- FP16 storage support and FP16 arithmetic support are distinct capabilities.
- Compiler flags that change floating-point contraction/semantics must not leak across target boundaries silently.

Every production precision reduction should have an explicit error budget and differential test.

## 7. Metadata and provenance policy

A computational-photography system is only as trustworthy as its metadata semantics.

Current precedence examples are explicit: optical-black estimates outrank dynamic black, which outranks static black; dynamic white outranks static white. More generally:

- a value should carry source, validity, and confidence when provenance matters;
- fallbacks should be deterministic and explainable;
- estimated/calibrated/device-profile values must not be indistinguishable from authoritative capture metadata;
- future burst-derived values should retain the frame/burst evidence that produced them;
- execution traces should record which candidate/fallback actually won.

## 8. Known structural gaps and design debt

These are architectural facts, not criticisms of the staged implementation.

### 8.1 `ImagingGraph` and `ImagingBackend` are competing seeds

`ImagingGraph` expresses a general typed pipeline; `ImagingBackend` currently exposes only `reconstructSingleRaw()`. Keeping both as permanent orchestration APIs would fragment control.

**Direction:** promote the graph/compiler/execution-plan path to the universal control plane. Keep high-level convenience APIs as thin request builders/adapters over that plane.

### 8.2 Operation traits are too caller-controlled

`OperationDescriptor` currently stores `access`, `pure`, `canFuse`, `temporal`, and `changesReferenceDomain` directly. An agent/caller can construct internally inconsistent descriptions.

**Direction:** introduce canonical operation schemas/traits derived from operation kind plus explicitly typed parameters. Make illegal descriptions unrepresentable or fail at construction.

### 8.3 Ingress and codec taxonomy need reconciliation

`RawIngress` exists as an `OperationKind`, while graph validation currently expects operations to have inputs and graph inputs are already a separate concept. `GainMapEncode` also remains in the imaging IR even though the implemented architecture puts gain-map/container semantics behind an external codec boundary.

**Direction:** reconcile external input/output nodes and codec ownership when the compiler IR is promoted. Do not use these early enum values as authority over the newer explicit boundaries.

### 8.4 Semantic descriptors are duplicated

`SceneFrame`, `RenderedFrame`, and `ImageType` repeat primaries/white/transfer/reference/range fields. This is useful during prototyping but increases drift risk.

**Direction:** converge on canonical semantic descriptors or validated type constructors, with frame payloads carrying/referencing one authoritative descriptor.

### 8.5 Execution is not yet plan- or trace-driven

`ComputeRunner` dispatches individual pipelines correctly but does not expose a first-class plan, resource lifetime graph, synchronization model, capability reasoning, or machine-readable execution trace.

**Direction:** create `ExecutionPlan` and `ExecutionTrace` before adding substantial burst/multi-pass complexity.

### 8.6 Capture and temporal semantics are missing

Android NDK capture contracts, recorded metadata fixtures, `RawBurst`, alignment, robust merge, temporal uncertainty, and multi-frame lifetime management are not yet implemented.

**Direction:** add these through new semantic objects/operations rather than by expanding `RawFrame` or Vulkan runner ad hoc.

## 9. Target control plane

A target request should eventually look conceptually like this:

```text
ProcessingRequest
  - capture/reconstruction intent
  - scene output requirements
  - render intents (0..N)
  - export intents (0..N)
  - quality/latency/power policy
          |
          v
SemanticGraphBuilder
          |
          v
validated SemanticGraph
          |
          v
GraphCompiler(semantic graph, DeviceCaps, device profile, policy)
          |
          v
ExecutionPlan
  - resources/lifetimes
  - fused groups
  - selected lowerings
  - precision/storage choices
  - barriers/external boundaries
  - reasons/fallbacks
          |
          v
Executor
          |
          +--> outputs
          +--> ExecutionTrace
```

The compiler should be deterministic for the same semantic graph + capability/profile/policy inputs. If heuristic/tuned choices are introduced, the chosen parameters and profile version must be recorded in the trace.

### 9.1 Why `ExecutionTrace` matters

A structured trace is one of the highest-value future features for both humans and agents. It should answer without source spelunking:

- which semantic graph/request produced this result;
- which metadata candidates and fallbacks were selected;
- which backend/lowering ran each operation;
- which capability gate caused a fallback;
- which storage/precision boundary was used;
- which device/profile/version was active;
- timing/memory counters when measured;
- validation fingerprints/tolerance class where relevant.

This makes performance and correctness debugging a data problem rather than a reconstruction of hidden control flow.

## 10. Extension rules

A new feature belongs in Latent only after answering these questions:

1. What semantic state does it consume and produce?
2. Does it change reference domain or only realization?
3. What metadata/provenance does it require and emit?
4. What is the deterministic reference behavior or external standard authority?
5. Where is the policy/mechanism boundary?
6. What precision/storage rules apply?
7. What production lowering(s) exist or are planned?
8. What evidence proves semantic equivalence or standards conformance?
9. Does it require a durable ADR?
10. Can an agent discover all of the above from one canonical place rather than several copied descriptions?

If those answers are unclear, the feature is not ready to become another execution path.

## 11. Documentation ownership

To prevent knowledge drift:

- this file owns **stable system architecture and invariants**;
- `docs/roadmap.md` owns **maturity, branch topology, and sequencing**;
- `docs/verification.md` owns **tests, evidence requirements, and commands**;
- `docs/decisions/` owns **durable decisions and supersession history**;
- `README.md` is only the concise project entry point;
- `AGENTS.md` is the concise operational contract and must link rather than duplicate deep design.

Architecture should not be reconstructed from a chain of PR descriptions. PRs deliver increments; the documents above preserve the coherent system.
