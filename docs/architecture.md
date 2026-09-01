# Latent architecture

## 1. Architectural thesis

Latent is a **semantic imaging compiler/runtime**, not a Vulkan library and not a bag of ISP stages.

The stable architectural order is:

> **semantic meaning first, explicit intent/observations second, compilation third, execution backend fourth, evidence throughout.**

A Vulkan resource, shader pass, codec API, Android handle, device profile, or performance heuristic may help realize an image operation, but none of them may silently define what the image means.

The system is easiest to understand as four coupled planes:

1. the **semantic plane** — what each image/value means and which transitions are legal;
2. the **authority/input plane** — what was observed, what is requested/delegated, and what the runtime can execute;
3. the **realization plane** — graph compilation, plans, resources, and executors;
4. the **evidence plane** — reference semantics, goldens, differential/integration tests, and traces that prove the realization preserved meaning.

These planes meet at every operation. Their separation is what allows the whole repository to behave as one system rather than a collection of locally reasonable components.

## 2. Semantic plane: image meaning changes only at explicit boundaries

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
    sensor-referred samples + capture observations/provenance
        ^
        | capture / ingress boundary
        |
L0  Camera2 / NDK Camera / recorded fixtures / external input
```

The important property is not the current struct names; it is that each state has a different meaning and therefore a different set of legal operations.

| Semantic state | Canonical current type | Legal responsibilities | Must not be hidden here |
| --- | --- | --- | --- |
| Sensor-referred | `RawFrame` | packing/stride interpretation, black/white selection, sensor normalization, defect/LSC/WB, noise/provenance handling | display tone, display transfer, final picture brightness |
| Scene-referred | `SceneFrame` | scene analysis, scene-linear reconstruction/denoise/merge, scene coordinate transforms | display clipping, sRGB/PQ/HLG encoding, codec gain-map policy |
| Display-referred | `RenderedFrame` | fixed render intent/exposure, tone/gamut decisions, output primaries, target white/peak, display transfer | rewriting capture radiometry or scene-master coordinates |
| Codec/export | `UltraHdrRenditionPair`, encoded bytes | packing, standards metadata, container behavior, external codec integration | redefining scene or rendering semantics |

### 2.1 `SceneFrame` is the architectural center

`SceneFrame` is the master computational image object. Current semantics are:

- linear ACEScg/AP1 primaries with D60 white;
- scene-referred;
- unbounded;
- negative coordinates are legal;
- `sceneScaleEV` is an explicit coordinate/representation scale;
- propagated noise and confidence/provenance data may accompany the scene image.

`sceneScaleEV` is **not** display white, middle gray, final image brightness, capture exposure, or tone-map exposure. Rendering removes this representational coordinate choice before applying the separately requested `renderExposureEV`.

### 2.2 Domain changes are typed operations, not conventions

A transition such as sensor -> scene or scene -> display must be visible in the graph/type system. The current `ImageType` and `ImagingGraph` already validate part of this: scene values must be linear/unbounded/negative-permitting, and operations that change `ReferenceDomain` must declare that change.

Target architecture should make illegal combinations difficult or impossible to construct. Prefer canonical semantic descriptors/constructors and operation schemas over repeatedly filling free-form fields correctly.

## 3. Authority/input plane: facts, intent, policy, and capabilities are different kinds of data

ADR-0003 defines a critical control-plane boundary. Values that look like ordinary configuration fields can have different authority:

| Class | Meaning | Examples | What it may influence |
| --- | --- | --- | --- |
| Semantic rules | versioned correctness/invariants | reference-domain rules, operation schemas, `SceneFrame` meaning, error requirements | legal graph and legal implementations |
| Observations/evidence | learned facts with provenance | Camera2 metadata, optical black, calibration, noise profile, device-profile measurements | semantic parameters through explicit validated selection/fallback rules |
| Image intent / delegated policy | requested result and explicitly allowed choices | render intent/exposure, requested outputs, fixed algorithm choice, quality/latency/power delegation | semantic request and compiler choices only where delegation is explicit |
| Execution capabilities | runtime/platform facts | Vulkan features, AHB import support, codec availability, supported formats | legal lowerings, resource strategy, explicit rejection/fallback |

Directional authority matters:

- a capability may select a lowering; it may not silently change fixed image intent;
- a policy default may not masquerade as captured/calibrated evidence;
- an observation may be selected/fallbacked through an explicit rule; it may not rewrite project invariants;
- a performance heuristic may alter observable semantics only when the request explicitly delegated that choice;
- any such selection/fallback should be explainable from plan/trace data.

This distinction is more useful than a universal configuration object. In particular, `ReconstructionConfig` is a pragmatic current adapter but mixes algorithm selection, calibration/observation-derived values, semantic parameters, and confidence. Do not expand that pattern into the final control plane.

## 4. Realization plane: what, how, and physical storage are separate

```text
semantic types + operation schemas
              |
              v
        SemanticGraph
              |
       validation / normalization
              |
     +--------+---------+
     | authority contexts|
     | observations      |
     | intent/policy     |
     | capabilities      |
     +--------+---------+
              |
              v
        GraphCompiler
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
             outputs + trace
```

`SemanticGraph`, `GraphCompiler`, authority-separated compiler contexts, and `ExecutionPlan` are architectural names. The repository currently has an `ImagingGraph` seed and direct reference/Vulkan entry points, but no complete compiler or first-class execution plan yet.

### 4.1 Operation schemas own intrinsic semantics

Operation schemas should own facts such as:

- input/output domains and type constraints;
- point/neighborhood/reduction/temporal/external access pattern;
- purity and fusion legality;
- precision requirements;
- whether reference domain changes;
- required observations/provenance;
- reference implementation availability;
- supported production lowerings and their capability requirements.

Today `OperationDescriptor` allows callers to provide several of these traits manually. That was appropriate for the first IR slice but is too permissive for a system an agent must reason about reliably. Intrinsic traits should be derived from canonical schemas; callers should provide true operation parameters and explicitly delegated choices, not reinvent operation truth.

### 4.2 Reference semantics are executable specification

The FP32 reference path is not merely a slow version of Vulkan. It is the semantic oracle against which production implementations are compared.

Reference code should optimize for determinism, explicit intermediate meaning, auditable provenance, reproducible precision, and golden/property-test friendliness. Production code may fuse operations, change storage precision, alias memory, or select device-specific paths only when the resulting behavior remains inside an explicit equivalence/error budget.

### 4.3 Semantic identity/lineage is not physical storage

ADR-0002 separates two questions:

- what semantic value is this and what source evidence produced it?
- where are its bytes currently stored and who owns that resource?

Current frame structs own CPU vectors because they are convenient host/reference values. They must not evolve by embedding Vulkan/AHardwareBuffer handles as semantic identity. Likewise, current scalar `sourceRawId` is a single-source convenience and must not be overloaded to encode future bursts or provenance graphs.

Target execution associates semantic value IDs/lineage with backend-neutral plan resources, then binds those resources to host buffers, Vulkan buffers/images, imported allocations, or external codec/platform objects.

### 4.4 Graph compiler is the missing hinge

The graph compiler should become the single control plane that answers:

- Which semantic operations and outputs are requested?
- Are their types/domains/required observations legal?
- Which observation candidate/fallback was selected and why?
- Which choices are fixed image intent and which were delegated to policy?
- Which operations may be fused without changing meaning?
- Which precision/storage boundaries are permitted?
- Which backend/lowering is legal on the current capability context?
- Where are reductions, temporal barriers, external interfaces, and reference-domain transitions?
- What are resource lifetimes and synchronization requirements?
- Why was a particular fallback, rejection, or fast path selected?

Without this hinge, `ImagingGraph`, `ImagingBackend`, direct reference functions, mixed configuration structs, and kernel wrappers can become competing orchestration surfaces.

### 4.5 `ExecutionPlan` is explicit mechanism, not semantic invention

A future `ExecutionPlan` should contain enough information to execute and diagnose a frame without reopening the entire source tree:

- stable semantic operation/value IDs and plan resource IDs;
- selected lowering for each semantic operation/group;
- resource format/extent/ownership/lifetime classes;
- semantic-value -> resource association;
- precision/storage decisions;
- dispatch dependencies/barriers;
- external ingress/egress boundaries;
- required/selected capabilities;
- delegated policy choices actually made;
- explicit fallback/rejection reasons;
- semantic operation references and tolerance/evidence class where relevant.

It must not invent creative image-policy decisions absent from the request or its explicit delegation.

### 4.6 Executors execute already-decided work

- The deterministic reference executor/path establishes semantic behavior.
- Vulkan is the primary production compute realization.
- Platform capture/display adapters and external codecs are integration executors/adapters at system edges.

`ComputeRunner` is currently a deliberately synchronous one-queue harness with host-visible buffers and one-pipeline dispatches. It is a correctness seed, not the final scheduler, allocator, or multi-frame executor.

### 4.7 Derived system introspection is a projection, not another authority

ADR-0004 adds a machine-readable introspection surface because agents should not have to scan enums, headers, CMake, wrappers, and tests to discover the implemented system. The target `SystemCatalog` (name provisional) is derived from the same canonical semantic/operation/lowering/diagnostic registries and build-feature facts that compilation uses.

It must remain separate from runtime `DeviceCaps`, observation/profile evidence, request/policy, roadmap maturity, and live GitHub delivery state. A combined inspection tool may display those views together, but it may not merge their authority.

The introspection/control artifacts also need explicit format/schema versions, deterministic canonical representations, and content fingerprints. Scoped identities such as semantic value/run/resource IDs are not content hashes. Compiler/validation diagnostics should become structured records with stable codes/fields; human-readable messages remain views rather than machine control data.

The key invariant is: **the catalog is generated from implementation truth; implementation truth is never reconstructed from a hand-edited catalog.**

## 5. Evidence plane: correctness and explainability are architecture

Every significant semantic operation should have an evidence chain:

```text
standard / paper / accepted invariant
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
        CI + ExecutionTrace
```

Evidence is how Latent permits aggressive fusion, FP16 storage, Vulkan execution, external integration, and later device specialization without losing semantic control.

A structured `ExecutionTrace` should make diagnosis a data problem rather than reconstruction of hidden control flow. It should be able to answer:

- which semantic request/run produced the result;
- semantic value identities and lineage;
- which observations/candidates/fallbacks were used and with what provenance/confidence;
- fixed intent and any policy choices delegated/made;
- compiler/profile/capability fingerprints;
- catalog/semantic-schema/compiler format identity needed to interpret the trace;
- which lowering ran each operation and why;
- which physical resource bound each plan resource;
- precision/storage decisions;
- rejection/fallback causes;
- measured timing/memory counters, explicitly separated from inferred claims.

For commands and evidence requirements, see `docs/verification.md`.

## 6. Current implementation mapped onto the system

### 6.1 Sensor model and single-RAW reference reconstruction

`RawFrame` carries canonical raw storage plus CFA, exposure information, black/white candidates, neutral/color gains, lens shading, noise profile, defects, and metadata source/validity/confidence where modeled.

Current reference flow:

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

The deterministic box-average demosaic remains a comparison baseline; Malvar-He-Cutler 2004 is the normal reference reconstruction choice.

### 6.2 Color science and uncertainty

`imaging/ColorScience` and `reference/DngColor` implement SMPTE RP 177 primary/XYZ derivation, Bradford adaptation, Robertson CCT, DNG Chapter 6 dual-illuminant interpolation, camera-neutral/white-balance conversion, ForwardMatrix and inverse-color-matrix paths, and XYZ D50 -> linear AP1/D60.

`reference/SensorLinearOps`, `imaging/Noise`, and `reference/NoisePropagation` provide map-driven defect correction/detection, Android-convention LSC, NOISE_PROFILE transformation, variance propagation through sensor/color stages, and lazy scene sigma queries. Monte Carlo validation is used because uncertainty propagation itself is a semantic contract.

### 6.3 Vulkan ingress and capabilities

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

Direct import is never a correctness prerequisite. Claims must distinguish shared allocation/handle availability, Vulkan import success, and measured traffic elimination. `DeviceCaps` is a capability context: it chooses legal realizations; it does not redefine the image.

### 6.4 Production Vulkan K1a/K1b

K1a (`sensor_preprocess.comp` + `SensorPreprocessKernel`):

```text
canonical uint16 RAW
 -> black subtraction
 -> white normalization
 -> lens shading
 -> white balance
 -> packed FP16 Bayer storage
```

K1b (`demosaic_color.comp` + `DemosaicColorKernel`):

```text
FP16 Bayer
 -> box or MHC demosaic (FP32 accumulation)
 -> camera-to-scene color transform (FP32)
 -> explicit scene scale
 -> packed RGBA16F scene storage
```

Computation/precision boundaries are differentially verified across CFA patterns, edge extents, LSC states, demosaic methods, sign preservation, NaN absence, and explicit error budgets.

### 6.5 Scene analysis and independent rendering

`render/SceneAnalysis`, `render/AcesToneScale`, `render/OutputEncoding`, and `render/ReferenceRenderer` establish the scene -> display boundary.

- analysis removes `sceneScaleEV` before deriving exposure statistics;
- rendering applies independent, fixed rendition intent such as `renderExposureEV`;
- SDR and HDR are independent branches from the same scene master;
- SDR target is Rec.709/D65/sRGB;
- HDR target is BT.2020/D65/PQ;
- the ACES-derived scalar tonescale is a luminance primitive only;
- current neutral-axis gamut compression is explicit and is **not** claimed to be the complete ACES 2 Output Transform;
- `RenderedFrame` is display-referred and records target peak, nominal white, and HDR headroom alongside encoded pixels.

Production Vulkan rendering remains planned.

### 6.6 Ultra HDR codec boundary

`codec/UltraHdrStaging` converts explicit SDR/HDR `RenderedFrame` values into packed inputs for libultrahdr:

```text
SceneFrame
  |-- SDR render -> Rec.709/sRGB -> RGBA8888 --|
  |-- HDR render -> BT.2020/PQ   -> RGBA1010102|
                                                v
                                          libultrahdr
                                 gain map + metadata + container
```

Gain-map math relates explicit display renditions and belongs to the codec integration layer, not RAW reconstruction or the scene master. The external dependency is isolated in `latent::codec` so its build/fast-FP policy cannot contaminate deterministic reference targets. JPEG integration is exercised; HEIF/AVIF still need complete dependency/platform validation.

## 7. Precision policy

- Reference reconstruction/rendering: deterministic FP32 behavior.
- RAW storage: packed/native integer until canonical ingress conversion.
- Current production scene storage target: RGBA16F where validated.
- Demosaic/color cancellation-sensitive math: FP32 accumulation.
- Future burst accumulation, motion/statistics, reductions, and similar numerically sensitive work: FP32 unless separate evidence justifies another choice.
- FP16 storage support and FP16 arithmetic support are distinct capabilities.
- Compiler flags that change floating-point contraction/semantics must not leak across target boundaries silently.
- Every production precision reduction needs an explicit equivalence/error budget and differential evidence.

Precision policy belongs to semantic correctness and explicitly delegated quality policy; it must not emerge accidentally from device convenience.

## 8. Metadata, observation, and provenance policy

A computational-photography system is only as trustworthy as its observation semantics.

Current precedence examples are explicit: optical-black estimates outrank dynamic black, which outranks static black; dynamic white outranks static white. More generally:

- a value carries source, validity, and confidence when provenance/uncertainty matters;
- fallbacks are deterministic and explainable;
- measured, captured, estimated, calibrated, device-profile, and policy-default values must remain distinguishable;
- future burst-derived values retain the frame/burst evidence that produced them;
- execution traces record which candidate/fallback actually won;
- missing evidence may cause explicit fallback/rejection but must not be hidden by relabeling a default as an observation.

## 9. Known structural gaps and design debt

### 9.1 `ImagingGraph` and `ImagingBackend` are competing seeds

`ImagingGraph` expresses a general typed pipeline; `ImagingBackend` exposes only `reconstructSingleRaw()`.

**Direction:** promote graph/compiler/execution-plan as the universal control plane. Keep convenience APIs as thin request builders/adapters.

### 9.2 Operation traits are too caller-controlled

`OperationDescriptor` stores intrinsic traits directly.

**Direction:** canonical operation schemas derive these facts from kind + typed parameters.

### 9.3 Ingress and codec taxonomy need reconciliation

`RawIngress` coexists with explicit graph inputs; `GainMapEncode` remains in the imaging enum despite the explicit external codec boundary.

**Direction:** reconcile boundary nodes when the IR is promoted; early enum values are not authority over accepted architecture.

### 9.4 Semantic descriptors are duplicated

`SceneFrame`, `RenderedFrame`, and `ImageType` repeat primaries/white/transfer/reference/range state.

**Direction:** converge on canonical descriptors/constructors with one authoritative semantic description.

### 9.5 Lineage and physical resources are under-modeled

Scalar `sourceRawId` and frame-owned vectors are sufficient for current single-RAW/reference work but not future burst/executor reasoning.

**Direction:** typed identities/source sets + separate execution resources/bindings (ADR-0002).

### 9.6 Configuration authority is mixed

Current request/config structs sometimes mix observed/calibrated parameters, fixed image intent, algorithm selection, confidence, and implementation-oriented choices.

**Direction:** authority-separated contexts (ADR-0003); do not create a universal bag of knobs.

### 9.7 Execution is not plan- or trace-driven

`ComputeRunner` correctly dispatches individual pipelines but exposes no first-class resource lifetime graph, capability reasoning, or structured trace.

**Direction:** create `ExecutionPlan`/`ExecutionTrace` before substantial burst/multi-pass complexity.

### 9.8 Capture and temporal semantics are missing

Android NDK capture contracts, recorded metadata fixtures, `RawBurst`, alignment, robust merge, temporal uncertainty, and multi-frame lifetime management are not yet implemented.

**Direction:** add them through new semantic/observation objects and temporal operations, not by expanding `RawFrame` or the Vulkan runner ad hoc.

### 9.9 Static system introspection and artifact compatibility are under-modeled

Today an agent must reconstruct implemented operations, reference paths, lowerings, optional build integrations, and many fallback reasons from several code/document surfaces. Some validation/decision APIs expose only free-form strings. There is no canonical build-wide catalog or explicit persisted-artifact compatibility model yet.

**Direction:** implement ADR-0004/Plan 0002 after canonical operation registries begin to land: derive `SystemCatalog`, structured diagnostics, canonical serialization, explicit versions, and fingerprints from authoritative registries rather than checking in another manifest.

## 10. Target control plane

Conceptually:

```text
SemanticRequest
  - capture/reconstruction semantic choices
  - scene outputs
  - fixed render intents (0..N)
  - export intents (0..N)
  - explicitly delegated automatic choices
            |
            +--------------------+
                                 |
ObservationContext              |
  - capture metadata            |
  - calibration/profile facts   |
  - provenance/confidence       |
                                 v
                         SemanticGraphBuilder
                                 |
                                 v
                         validated SemanticGraph
                                 |
IntentPolicy -------------------+
  - quality/latency/power       |
  - only delegated choices      |
                                 v
CapabilityContext ----------> GraphCompiler
  - DeviceCaps                  |
  - platform/codec facts        |
  - measured profile facts      |
                                 v
                          ExecutionPlan
                     - semantic/resource IDs
                     - selected lowerings
                     - precision/storage
                     - lifetimes/barriers
                     - policy/capability reasons
                     - fallbacks/rejections
                                 |
                                 v
                              Executor
                                 |
                         +-------+-------+
                         v               v
                      outputs      ExecutionTrace
```

The compiler should be deterministic for the same semantic graph/request + observation/profile versions + policy + capabilities. If tuned/heuristic choices are introduced, the chosen parameters, delegation, evidence/profile version, and reason must be recorded.

The canonical registries used by graph construction/compilation also derive the static `SystemCatalog`; the compiler does not read a separately maintained catalog. Plans/traces should carry enough schema/catalog/compiler version or fingerprint context to make their interpretation explicit across builds.

The target is not “one giant object containing everything.” The target is one explicit compilation boundary whose inputs retain their authority and whose output explains every realization decision, plus one cheap derived introspection view that lets an agent discover what the implementation contains.

## 11. Extension rules

A new feature belongs in Latent only after answering:

1. What semantic state does it consume and produce?
2. Does it change reference domain or only realization?
3. Which inputs are semantic rules, observations, fixed intent, delegated policy, and capabilities?
4. What observations/provenance does it require and emit?
5. What is the deterministic reference behavior or external standards authority?
6. What precision/storage/equivalence rules apply?
7. What production lowerings exist or are planned?
8. What evidence proves semantic equivalence or standards conformance?
9. What lineage/resource implications exist?
10. Does it require a durable ADR?
11. Can an agent discover the answers from canonical sources without reconstructing them from PR history or hidden control flow?
12. Once the introspection surface exists, will the feature appear automatically through canonical registrations, diagnostics, and evidence linkage rather than a manual manifest update?

If these answers are unclear, the feature is not ready to become another execution path.

## 12. Knowledge ownership and freshness

Agent-friendliness depends as much on **not storing stale facts** as on storing useful design.

Route knowledge by volatility:

- this file owns **stable system architecture and invariants**;
- `docs/decisions/` owns **durable decisions, rationale, and supersession history**;
- `docs/roadmap.md` owns **capability maturity, dependencies, and sequencing**, not exact live branch state;
- `docs/plans/` owns **active multi-step implementation state and acceptance criteria**;
- `docs/verification.md` owns **evidence requirements and reproducible validation commands**;
- canonical implementation registries own **implemented semantic/operation/lowering truth**; the future `SystemCatalog` is their generated machine-readable projection, not a separate authority;
- runtime capability queries own **live device/platform facts**;
- GitHub owns **volatile delivery facts**: current branches, PR open/merged state, head SHAs, review threads, workflow/check results;
- `README.md` is a concise project entry point;
- `AGENTS.md` is the low-context operating/router contract and links rather than reproduces deep design.

Do not reconstruct architecture from a chain of PR descriptions. Do not reconstruct live delivery state from a dated architecture/roadmap snapshot. Do not reconstruct implementation truth from a hand-edited catalog. Query or derive the authority appropriate to the question.