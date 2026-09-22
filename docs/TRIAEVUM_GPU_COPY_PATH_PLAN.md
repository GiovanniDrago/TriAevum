# Piano: TextureCopy raw GPU/NRI e consumer GPU-owned

## Stato e obiettivo

Documento di design, non implementazione. Nessun sorgente del repository modificato e nessuna nuova esecuzione del gioco/build in questo passaggio. I riferimenti sotto sono stati verificati leggendo il checkout; le righe possono spostarsi per lavoro concorrente. I nomi marcati **nuovo** sono proposte, non API già disponibili.

Obiettivo: mantenere la copia raw e il successivo campionamento sul percorso GPU/NRI, integrando sistematicamente gli effetti nativi nella ricomposizione proprietaria del frame. Non implementare un nuovo effetto desaturazione: il draw nativo continua a produrlo tramite shader/state canonici. Non riconoscere scene, asset, hash di shader, checkpoint o indirizzi specifici nel renderer.

Vincoli: `AGENTS.md:5-15`; `docs/OOT3D_NRI_FIDELITY_EXTENSION_ARCHITECTURE.md:9-12,36-49,72-79,111,179-182`. Il composer mantiene l'ordine nativo; il grafo possiede dipendenze, barriere e lifetime; gli effetti opzionali non possono cambiare target, shader, state o semantica dei transfer. La copia raw è un'operazione nativa obbligatoria, non un effetto opzionale. Il percorso AOT/guest rimane l'oracolo end-to-end, senza fallback interprete.

Le sezioni 7.2 e 7.4 di `docs/TRIAEVUM_ALPHA2C_TO_ALPHA3_RECONSTRUCTION.md` sono state lette per prime. L'ordine tecnico concordato è copia GPU, binding GPU, materializzazione guest solo necessaria. La verifica AOT D è un prerequisito della scelta di coerenza e del cutover, non un'indagine rimandata a dopo il rilascio.

## 1. Evidenze e distinzione dei tre percorsi

### 1.1 Percorso caldo realmente da sostituire

`runtime/three_ds_recomp/src/fast/backends/gfx_vulkan_pica_texture_copy.cpp`:

- `SubmitNativePicaTextureCopy`: identità/dedup corrente a 48-56; byte count allineato a 16, width/gap e limiti a 57-67; owner sorgente unico a 68-78; rifiuto destinazione attachment attivo a 79-86.
- A 88-106 termina il render pass, chiama `WaitForAllPresents`, sottomette il prefisso e fa `vkQueueWaitIdle`, quindi `CaptureNativePicaImageBytes`.
- A 107-108 `EncodePicaFramebuffer` converte su CPU; a 112-129 distrugge le texture sovrapposte, operazione oggi sicura solo perché la queue è stata svuotata.
- A 137-160 costruisce span di writeback, legge i vecchi byte guest e chiama `mPhysicalMemoryCommit`, pubblicando poi `mRawTextureCopyWritebacks`.
- `ResolveNativePicaCopiedTexture`, righe 8-39, corregge snapshot CPU vecchi solo se corrispondono a `Before` e la memoria live corrisponde ai byte scritti. Non è già un resolver di risorse GPU.

La funzione di readback è `gfx_vulkan_pica.cpp:1865-1929`: buffer host-visible, comando immediato, image-to-buffer, ritorno di `vector<uint8_t>`. Questo giro non deve restare nella copia ordinata.

### 1.2 Snapshot esplicite da CONSERVARE

In questo checkout le altre chiamate pertinenti sono:

- `gfx_vulkan_pica.cpp:8348-8431`, `CapturePicaTextureCache`, chiamata a 8406; richiede frame idle e cattura i mip per snapshot.
- `gfx_vulkan_pica.cpp:8555-8646`, `CapturePicaColorTargets`, chiamate a 8590 e 8630; cattura color/guide/shadow per snapshot.
- `CapturePicaTextureCache:8361-8364` esclude già gli alias Shadow2D, serializzati tramite il target proprietario.

Queste non sono chiamate di normale sampling live. Non eliminarle per dimostrare “zero readback”. Devono restare funzionanti come materializzazione esplicita per savestate/capture/debug. Le risorse raw GPU-owned nuove dovranno essere incluse nella snapshot del loro owner, senza duplicare gli alias; restore deve ricostruire contenuto e identità, mai serializzare handle Vulkan/NRI. Le attese esplicite a un confine di snapshot non sono la via dell'effetto.

### 1.3 Il seam effettivo di sampling

- `tools/oot3d/native_game_runtime/oot3d_native_pica_submission.cpp:388-495`, `CaptureTextureResources`: legge `mMemory.View`, confronta/copia byte guest e produce snapshot immutabili; a 405-449 è evidente il percorso CPU.
- `oot3d_native_pica_submission.h:77-99`, `Oot3dPicaResourceSnapshot`: byte condivisi, `ContentVersion`, hash base/full separati.
- `oot3d_native_pica_vulkan_plan.h:58-73`, `Oot3dPicaVulkanTextureBinding`: trasporta ancora `NativeBytes`/`SharedNativeBytes`.
- `oot3d_native_pica_vulkan_bridge.cpp:224-235` costruisce `PicaTextureView`; il contratto condiviso sta in `runtime/three_ds_recomp/include/fast/renderer3ds/pica_native_state.h:150-170`.
- `gfx_vulkan_pica.cpp:4137-4165`: `ResolveNativePicaCopiedTexture` prima di `GetOrCreateNativePicaTexture`.
- `GetOrCreateNativePicaTexture:2553-2579` costruisce identità da byte/hash; a 2749-2794 Shadow2D usa già un alias di immagine live, con sampler/view propri e immagine del target in `NriImage`. Riutilizzare questo modello di borrowing, non il suo criterio incompleto di lookup e non il valore `GENERAL` come surrogato di ownership.

## 2. Contratto comune e ordine di implementazione

Ordine operativo: **D1a/D1b (rendere identificabili le copie), baseline misurata -> A -> B -> C -> D2/D3/D4 (classificazione del consumer e prova avversaria) -> end-to-end -> gate finale**. Le fasi A/B sono incrementi di sviluppo, non prodotti intermedi distribuibili con memoria guest stale. A, B e C devono essere integrate prima di attivare il cutover sul gioco. D2/D3 richiedono che A e C producano misurazioni/artefatti tracciabili; D3 non dipende dalla modifica dell'effetto ma dalle copie reali individuate in D1.

### 2.1 Un owner, due tipi di visibilità

L'owner fisico delle nuove superfici è il registro del backend al seam oggi denominato `mRawTextureCopyWritebacks` (`gfx_vulkan.h:823-831`). Il registro diventa un registro di versioni GPU, non una seconda copia di guest RAM. Conservare il seam per il cutover; rinominare il record se necessario, senza tenere alias API deprecati. Il grafo decide produzione, letture, barriere ed export/lifetime; NRI è l'allocatore/esecutore, non un owner logico concorrente. I record texture e gli effetti ricevono binding in prestito.

Identità logica di superficie: epoch del contenuto/restore + render-target namespace + base/intervalli fisici + layout nativo completo (dimensioni, formato, tiling, stride e intervalli validi, mip/layer quando presenti). Identità di versione: seriale monotono per l'owner, producer `CompletionId`, confine `AfterDrawSubmissionId`, generazione sorgente. Il layout Vulkan corrente è stato della risorsa, NON parte mutevole della chiave. Sampler/wrap/filter/LOD appartengono al binding, non alla proprietà dei byte. Render scale/sample count/extent fisico appartengono alla rappresentazione della sorgente e alle condizioni di conversione, non cambiano gli indirizzi guest.

Distinguere:

1. **GPU-visible:** il producer è registrato/sottomesso con dipendenza che precede il consumer GPU;
2. **GPU-retired:** fence/token del lavoro produttore completato;
3. **CPU-materialized:** gli intervalli necessari sono stati riportati nella memoria guest;
4. **PPF-delivered:** obbligo guest assolto una volta.

Non usare un solo booleano `Completed` per questi stati.

La chiave attuale `(namespace, outputAddress)` non basta per layout alternativi, overlap parziali, versioni conservate e reuse dopo restore. Il controllo di overlap è sugli intervalli fisici anche quando due viste hanno layout diversi: aggiungere il layout alla map senza invalidazione trasversale non risolve la coerenza. Non usare un content hash di byte guest vecchi come identità GPU.

### 2.2 Native raw bytes, non un blit estetico

Il transfer raw non dichiara una texture destinazione: i campi width/height sono width/gap in unità di 16 byte. Non inferire una dimensione 2D da `OutputWidth/OutputHeight` del transfer. Il layout campionabile proviene dal successivo contratto texture, o da una superficie già registrata.

Riutilizzare `BuildOot3dPicaTextureCopyPlan` e `BuildPicaTiledTextureCopyRegions` (`renderer3ds/pica_texture_copy_plan.h:12-39,45-69,71-128`): il planner shared già traduce Morton in regioni 2D, conserva gap e rifiuta formato diverso/texel parziali. Il commento a 32-35 vieta precisamente di trasformare reinterpretazione raw in un blit filtrato.

Rappresentazione minima:

- Se layout sorgente/destinazione e codifica sono noti e la mapping native→image è esattamente equivalente, una immagine GPU-owned copiata tramite regioni è sufficiente.
- Altrimenti serve backing GPU byte-oriented degli span raw e conversione compute verso la vista campionabile richiesta. Questo è necessario quando il layout destinazione è ancora sconosciuto, per texel parziali o reinterpretazione. È una rappresentazione della stessa versione, non un owner aggiuntivo. Non mantenere inutilmente sia backing raw sia immagine se una sola rappresentazione basta; mantenere entrambe solo quando un consumer byte-oriented o layout alternativo lo richiede.
- La conversione source RGBA interno→byte nativi deve riprodurre `EncodePicaFramebuffer` (`pica_framebuffer_encode.h:10-44`): center sample intero a 25-26, Morton a 28-33, canali/quantizzazione formati 0..4 a 34-40. Un semplice image copy tra RGBA8 espansi non è equivalente per RGB565/RGB5A1/RGBA4 se salta la quantizzazione.
- Per una nuova destinazione, i byte non scritti/gap provengono dalla versione precedente o, se CPU-owned, da un upload dei soli intervalli necessari. Non clear impliciti, non zero-fill al posto dei byte guest. Upload CPU→GPU di dati realmente CPU-owned è ammesso; GPU→CPU→GPU per alimentare l'effetto no.

Non estendere silenziosamente la semantica dei casi attualmente rifiutati. Owner ambiguo, sorgente GPU assente, destinazione attachment attivo, range unmapped/wrap, NRI non disponibile, formato/MSAA/layout non implementato: errore esplicito con `CompletionId`, indirizzi, layout e causa; nessun PPF e nessuna ricaduta sul vecchio executor CPU. Una classificazione CPU-owned legittima resta distinta da un fallimento di una copia già assegnata alla GPU.

### 2.3 Grafo e ordine senza un nuovo framework

Riutilizzare `CompileExtensionResourceGraph` e le dichiarazioni esistenti (`renderer/extension_resource_graph.h:11-79,81-113`). Accetta una lista GIÀ ordinata; l'adapter mantiene l'ordine nativo. Dichiarare la sorgente come import esterno con initial barrier managed, la versione di copia come output, e le letture dei consumer come dipendenze. I confini nativi sono in `oot3d_native_pica_presentation_scheduler.cpp:223-288`: transfer dopo il draw produttore, prima del successivo draw consumer. Non riordinare per raggruppare copie, non spostarle a fine frame.

La compilazione risorse di copia si integra al confine nativo già esistente, non come effetto opzionale che può riscrivere target canonici. Gli effetti di ricomposizione importano il binding corretto della versione attraverso `PicaSceneFrame`/risorse dichiarate, senza un lookup occulto di “ultima immagine”. `EffectResourceBinding` ha già Identity/Generation/NativeImage/NativeView (`oot3d/display_effect_resources.h:33-44`). Non creare un enum di risorsa per ciascun indirizzo né un secondo scheduler.

Le versioni cross-frame/replay sono export persistenti, non slot transient dell'arena degli effetti: il backend conserva il backing finché esistono consumer registrati, replay/snapshot immutabili o GPU in-flight. Le barriere fisiche e il retirement sono prodotti dal piano, non dedotti dal fatto che un comando è stato registrato.

## 3. Fase A — copia GPU→GPU

### File/simboli

- `src/fast/backends/gfx_vulkan_pica_texture_copy.cpp`: `SubmitNativePicaTextureCopy`, `ResolveNativePicaCopiedTexture` (pubblicazione metadata; consumo completo in B).
- `include/fast/backends/gfx_vulkan.h:353-389,423-425,823-831`: record, chiavi/layout/versioni, fence/lifetime; riuso delle generazioni superficie esistenti.
- `src/fast/backends/gfx_vulkan_pica.cpp`: rilascio risorse in `DestroyNativePicaShaderResources` (426-429), reset/restore che oggi svuota il registro (8986-8988); niente riscrittura della classificazione Screen.
- `include/fast/renderer3ds/pica_render_backend.h:122-142,244-248`: contratto transfer/visibilità. Portare esplicitamente anche l'obbligo `SignalInterrupt` dal submission, oggi assente dalla view, senza emetterlo nella fase di registrazione.
- `tools/oot3d/native_game_runtime/oot3d_native_pica_vulkan_bridge.cpp:411-442`: propagazione completa del transfer, invariati campi raw/ordine.
- Riutilizzare, normalmente senza modificarli: `pica_texture_copy_plan.h`, `pica_framebuffer_encode.h` come oracle CPU; `oot3d/nri_interop_context.h:70-107` per owned texture/wrapper; `src/fast/oot3d/nri_interop_internal.h:22-59`, `NriInteropAccess::{Core,CommandBuffer,Texture,Buffer,CmdTextureBarrier,CmdGlobalBarrier}`. Il kernel compute specifico della conversione può restare nel modulo copia esistente; nessuna libreria generica nuova.
- Se il wiring del grafo richiede dichiarazioni aggiuntive, farlo nel modulo copia e nel punto di scheduling nativo citato, usando `ExtensionResourceGraphInput`; non cambiare le regole del compilatore condiviso per accettare una dipendenza non valida.

### Implementazione e barriere

1. Validare TUTTI gli span, mappature, formati e dipendenze prima di pubblicare una nuova versione o segnalare successo.
2. Chiudere il render pass necessario; registrare la copia nel command stream della graphics queue già usata, nello stesso punto nativo. Eliminare dal metodo copia attese present/global queue, immediate readback, conversione CPU e commit guest.
3. Sorgente attachment: `COLOR_ATTACHMENT_OUTPUT/WRITE -> TRANSFER/READ` per image copy oppure `COMPUTE/SHADER_READ` per encode. Destinazione nuova `UNDEFINED -> TRANSFER_DST`/storage write SOLO se non si perde contenuto da preservare; destinazione derivata dalla versione precedente passa dal suo stato effettivo.
4. Encode compute→raw copy e raw copy→decode: shader-write/transfer-write verso lettura successiva con barriera reale; byte storage GPU non deve richiedere funzionalità 8-bit storage non già supportate: usare packing in word con gestione esatta dei bordi, o dichiarare il requisito e rifiutare quando assente.
5. Output image: transfer/storage-write→fragment sampled-read, `SHADER_READ_ONLY_OPTIMAL` quando appropriato. Ripristinare lo stato sorgente richiesto dal prossimo uso canonico. Nessun layout inventato perché la texture è “una copia”. Usare gli state tracker/NRI già presenti.
6. Pubblicare identità + token di disponibilità dopo la registrazione riuscita. I consumer possono riferire una versione in-flight ma devono dipendere dal token. Se serve avanzare il guest, sottomettere il prefisso con fence/token specifico; non reset di un command buffer ancora pending. Il continuation buffer non può riusare risorse/descriptors del prefisso in-flight.
7. Rimuovere l'immediata `DestroyTexture` a 112-129: invalidare il lookup per i nuovi draw, ritirare vecchie texture/descriptors/backing solo al completamento dell'ultimo uso. Una versione per ogni frame non deve accumularsi senza limite.

### Acceptance A

- Oracle transfer e bridge C1 verdi senza cambiare expected data per farli passare.
- Prova GPU diretta C2: byte letti tramite materializzazione diagnostica ESPLICITA identici a `EncodePicaFramebuffer` + piano raw, inclusi gap, formati implementati, off-base, più layout e scale interne. Non basta che passi un planner CPU.
- Trace C3 mantiene la stessa sequenza `draw producer -> raw copy -> draw consumer`; nessun present inventato.
- Diagnostica C4: conteggio readback ordinati, attese queue/device/present sul percorso copia = 0; nessun errore Vulkan/NRI; unsupported = errore, zero completamenti. Il controllo statico dei simboli nel metodo è complementare, non prova sufficiente.
- Nessun incremento di memoria non limitato al numero di versioni effettivamente in-flight/pinned, verificato sulla run lunga e dopo retirement.
- Fase non distribuibile senza B/C/D: non dichiarare riuscita una copia GPU lasciando il consumer CPU stale.

## 4. Fase B — binding delle superfici GPU-owned

### File/simboli

- `oot3d_native_pica_submission.{h,cpp}`: `Oot3dPicaResourceSnapshot`, `CaptureTextureResources`, stato delle copie già dichiarate dalla submission; `SubmitDisplayTransfer:600-663` registra la promessa ordinata del producer anche prima che il backend la esegua.
- `oot3d_native_pica_vulkan_plan.{h,cpp}`: `Oot3dPicaVulkanTextureBinding`, costruzione/identità del piano.
- `oot3d_native_pica_vulkan_bridge.cpp:224-235`: passaggio del riferimento di versione al backend.
- `renderer3ds/pica_native_state.h:150-170`: `PicaTextureView`; aggiunta minima di un riferimento tipizzato a superficie/versione e provenienza, non handle Vulkan nel frontend.
- `gfx_vulkan_pica_texture_copy.cpp:8-39`: resolver della versione/layout corretti.
- `gfx_vulkan_pica.cpp:2553-2794,4137-4165`: early resolution GPU-owned PRIMA di hashing/decode/replacement da byte CPU; creare binding borrowed con sampler canonico.
- `oot3d_native_pica_visual_frame.cpp`, `oot3d_native_pica_visual_savestate.cpp`, `oot3d_native_pica_semantic_trace.cpp:426-458`: identità dei piani/replay e trace devono distinguere riferimento GPU da payload CPU; mai inventare un content hash da un seriale.
- `PicaSceneFrame::Record` e binding risolti in `src/fast/oot3d/pica_scene_frame.cpp`/`include/fast/oot3d/pica_scene_frame.h:44-92,166-186`: far arrivare la risorsa/versione reale ai consumer della ricomposizione, senza toccare `HasSceneDrawOnColorTarget:195-196` né la classificazione Screen.

### Ownership/coerenza

- Uno snapshot draw GPU-owned pinna una VERSIONE precisa, o il producer ordinato che la produrrà, non “l'ultimo output a quell'indirizzo”. Un draw precedente alla copia deve continuare a vedere il vecchio contenuto; un draw successivo la nuova versione; un'altra copia successiva non cambia retroattivamente il primo snapshot.
- `CaptureTextureResources` non legge byte guest stale per riconoscere la versione GPU. Riceve dal seam di submission/proprietà un riferimento producer+layout+epoch. La promessa logica del frontend e il backing del backend descrivono la stessa versione; non sono due owner.
- GPU-owned: prima lookup versionato, poi view/descriptors, barrier e sampling; niente decode/upload da CPU e niente `CaptureNativePicaImageBytes`. Il modello Shadow2D è il precedente per borrowing, non autorizza alias live mutabili per snapshot immutabili.
- CPU-owned: mantenere l'attuale snapshot immutabile e cache per contenuto quando nessun producer GPU copre il range. Ogni scrittura CPU sovrapposta dopo la copia ha precedenza per i byte scritti, anche se riscrive il medesimo valore. I restanti byte conservano la versione GPU precedente. Un upload delle nuove porzioni CPU produce una versione coerente; non invalidare tutta la superficie fingendo che la guest RAM contenga anche le porzioni ancora GPU-owned.
- `NativeA32Memory::RangeWriteGeneration` restituisce attualmente il token GLOBALE (`oot3d_native_a32_memory.cpp:220-230`), non un token per intervallo. Può segnalare che occorre ricontrollare, ma non è prova che una specifica superficie sia stata scritta né basta per un merge parziale. Non promettere tracking per-page già esistente. Il confine D/C deve intercettare scritture e letture GPU-owned in modo completo; finché non è provato, non attivare la GPU ownership.
- Le trasformazioni/replacement basate su hash CPU non devono leggere dati stale o trattare versioni GPU come asset immutabili. CPU-owned asset mantengono il comportamento esistente. Per contenuto generato GPU utilizzare l'identità superficie; eventuali consumer che richiedono davvero i byte/hash devono dichiarare una materializzazione esplicita, non introdurla occultamente nel draw. Non alterare la cache degli shader canonici: pipeline/shader keys rimangono distinte da texture generation.
- Mip completi: un transfer non genera mip nuovi per convenienza. Preservare quelli non scritti, invalidare le viste sovrapposte al livello giusto, rispettare i mip richiesti; mismatch non rappresentabile è errore.

### Snapshot e barriere

Un binding non possiede l'immagine; possiede solo view/sampler e una pin alla versione. Le transizioni producer→consumer appartengono al piano A; un consumer non può campionare un'immagine ancora attachment writable senza una transizione/risoluzione valida. Namespace di replay/interpolazione non devono mescolare superfici.

`CapturePicaTextureCache`, `CapturePicaColorTargets`, presentation snapshot e visual replay devono continuare a funzionare. Al confine idle materializzare tutte le versioni necessarie al restore una sola volta tramite l'owner, poi serializzare alias/referenze in modo coerente. Se basta normalizzare in payload CPU nello schema corrente, preferirlo a un nuovo formato savestate. Se nuove identità persistenti non sono ricostruibili dallo schema corrente, aggiornare encoder/decoder insieme con versione/compatibilità esplicita; non scartare dati sconosciuti in silenzio. Restore ripristina un epoch nuovo per gli handle runtime e la stessa obbligazione logica guest.

### Acceptance B

- C1 submission/plan/bridge verdi; nuove verifiche mirate di versioni immutabili: draw prima/dopo, due copie allo stesso indirizzo, namespace separati, scrittura CPU parziale e same-value, mip superiore modificato.
- C2/C3 confermano texture campionata post-copy e contenuto invariato dei draw già catturati; binding GPU-owned non attraversa decode/readback CPU.
- C5 boot/file-select byte-identici; C3 flashback copre tutte le colonne a 1280 e non regredisce rispetto alla baseline Screen già corretta.
- C6 snapshot/restore texture, color target e visual replay rimangono funzionanti. **Nessuna snapshot idle viene eliminata.** Verificare separatamente che la cattura esplicita incrementi i relativi readback, mentre la stessa run senza snapshot non ne incrementa per la copia/sampling.
- Niente hash di byte stale, nuovi AOT shader miss/reject o modifiche canoniche causate dalla nuova identità.

## 5. Fase C — materializzazione guest necessaria e PPF exactly-once

### File/simboli

- `renderer3ds/pica_render_backend.h:244-248,364-365`: oggi `PhysicalMemoryCommit` unisce scrittura e completamento. Separare i contratti minimi di materializzazione e consegna completamento, senza introdurre un servizio generico nuovo.
- `oot3d_native_a32_window.cpp:4403-4418`: oggi la callback ignora il `uint64_t` completion id, scrive i byte e segnala PPF. Deve usare identità/epoch e distinguere writeback da interrupt; mantenere il gate a 5312-5319 e la distinzione raw/non-raw a 5791-5802.
- `oot3d_native_pica_visual_frame.{h,cpp}`: `HasPendingRawCopyCompletion:1579-1591`, stato accumulator persistito; `oot3d_native_pica_presentation_scheduler.{h,cpp}`: `TakeDependencyWork`, `Execute`, delega a 370-371; `oot3d_native_pica_visual_savestate.cpp`: serializzazione dell'obbligo fino all'effettivo completamento.
- `gfx_vulkan_pica_texture_copy.cpp`/`gfx_vulkan.h`: token GPU e span dirty/materialized; poll/drain dal confine dependency, non queue idle.
- Se D richiede intercept: `oot3d_native_a32_memory.{h,cpp}` (`ReadFast`, `ReadBytes`, `GetReadPointer`, `WriteFast`, `GetWritePointer`, scritture checked/atomiche), `oot3d_native_a32_memory_region.h` (`NativeA32UnobservedMemory::{Resolve,ReadFast,WriteFast,Commit}`), e generatori AOT soltanto dove necessario a chiudere il bypass. Questo tocca ABI/plugin: qualificazione obbligatoria D.
- `oot3d_native_a32_savestate.cpp`: barriera di snapshot memoria prima della serializzazione già esistente, non un guest read simulato.

### Decisione di materializzazione, dipendente da D

1. Consumer esclusivamente GPU, con confine che rileva future letture/scritture CPU: nessun writeback per il sampling; PPF dopo completamento del lavoro ordinato e visibilità logica garantita dal contratto di memoria.
2. Lettura host pointer intercettabile: prima di restituire/accessare il puntatore, materializzare gli intervalli necessari della versione dovuta. Raggruppare solo richieste compatibili già note, mantenendo ordine e versioni. Attendere soltanto fence/token produttore; staging persistente/riusato e readback degli span necessari, non capture dell'intero render target. Dopo PPF un read deve vedere dati corretti; non assumere che PPF equivalga già a RAM fisicamente aggiornata se un intercept completo la rende equivalente.
3. Puntatore vivo/fast path che non può essere intercettato: materializzare gli intervalli provati host-observable prima del primo accesso possibile (al più tardi prima di PPF/ripresa guest che lo abilita). È writeback eager al confine guest necessario, non la via del sampling. Se quel confine non è identificabile senza riscrivere materializzazione AOT, bloccare il cutover e cambiare il lowering/ABI prima di procedere. Non ripiegare silenziosamente sul vecchio readback per ogni copia.

Batch soltanto entro lo stesso confine di visibilità: non posticipare una copia oltre un read, un PPF richiesto o un'altra versione dipendente. Writeback tardivo non deve sovrascrivere scritture CPU più recenti. Per read-modify-write CPU o pointer writable parziale prima materializzare le porzioni da preservare, poi assegnare la nuova versione. I writeback GPU non devono essere conteggiati come una nuova scrittura guest che nasconde il proprio producer.

### Exactly-once e fix esistenti

Identità dell'obbligo: epoch guest/replay + `CompletionId`, non semplice coppia outputAddress/namespace. Una replica grafica dello stesso transfer in namespace interpolato non deve generare un secondo interrupt. `SignalInterrupt=false` deve restare tale attraverso la bridge.

Transizioni: pending persistito -> GPU recorded/submitted -> GPU complete -> eventuale materializzazione richiesta -> PPF delivered. Un errore prima dell'ultima fase non deve registrare delivered. Una materializzazione successiva di byte già GPU-complete non deve emettere PPF di nuovo.

Il gate `HasPendingRawCopyCompletion` e il fix successivo a `0bcd22d` non vanno rimossi o riportati a latch window-local. Oggi `TakeDependencyWork` drena la lista: con completion asincrona occorre trasferire atomicamente l'obbligo a stato in-flight persistito, oppure completarlo entro la dependency flush con attesa mirata. Non lasciare una finestra savestate in cui la lista è vuota ma PPF non è ancora consegnato e l'obbligo esiste solo nel backend/window. Scelta minima iniziale: attesa mirata del token dentro dependency flush quando il guest necessita completamento subito; asincronia estesa solo se il modello persistito la copre.

Savestate: contenuti e obblighi devono essere una snapshot consistente; se il capture drena la GPU prima di serializzare, registrare anche lo stato PPF risultante una volta sola. Restore pending esegue una sola consegna; restore delivered non la ripete. Il valore diagnostico `pending_gpu_completions` oggi è letteralmente 0 (`oot3d_native_a32_window.cpp:7322-7324`): renderlo reale se si introduce in-flight osservabile, non usarlo come prova attuale.

### Acceptance C

- C1 visual-savestate continua a provare il pending restore e drain una volta (`oot3d_native_pica_visual_savestate_tests.cpp:228-255`). Aggiungere prova comportamentale del caso recorded ma non retired, completion duplicata e restore dopo delivered; non basta il totale aggregato PPF, che include altri transfer.
- C6 sui due checkpoint: sequenza avanza senza stallo, stessa progressione guest, zero duplicate draw attempts/faults, nessun PPF anticipato o duplicato per identità. Non richiedere stesso totale PPF fra run con durate guest differenti.
- C2/D4: una lettura guest immediatamente dopo completamento vede esattamente i byte nuovi; read parziale/cross-page e CPU write successiva non sono persi; materializzazione ripetuta non ripete PPF.
- C4 senza capture: zero materializzazioni per copie provate GPU-only; numero/byte di materializzazione motivati da accessi CPU effettivi o snapshot esplicite, zero global idle nel percorso copia. Le attese mirate vanno misurate: non occultare costo spostato in guest read.

## 6. Fase D — indagine AOT obbligatoria e falsificabile

### Evidenza già disponibile, non conclusione sul consumer

- `oot3d_native_a32_memory.h:88-125`: `ReadFast` legge direttamente dalla page table con `memcpy`; agganciare solo `GetReadPointer` non intercetta questo percorso.
- `oot3d_native_a32_memory.cpp:424-445`: getter restituiscono puntatori nelle regioni; il writable incrementa il token quando restituisce il puntatore, non a ogni uso futuro.
- `oot3d_native_a32_memory_region.h:39-71,151-172`: regioni unobserved conservano viste/puntatori e differiscono il commit delle scritture.
- `tools/oot3d/native_a32_runtime/whole_aot_cpp.py:925-950,1011-1025,1080-1098,1178-1183`: lowering di load/store usa `context.Memory.ReadFast/WriteFast`.

Canali diagnostici esistenti, verificati campo per campo:

- Il trace JSONL `--pica-semantic-trace` (`RecordDisplayTransfer` in `oot3d_native_pica_semantic_trace.cpp:338-363`; `WriteEvent:676-679`; flush unico a 669) **non** emette `texture_copy_bytes`. Emana: `completion_id`, `after_draw_submission_id`, `input_address`, `output_address`, `input_physical_address`, `output_physical_address`, `input_size`, `output_size`, `flags`, `signal_interrupt`, `selected_for_top_at_submission`, `topscreen_backdrop_suppression_active`. Non applica cap o finestre: è storia completa del run.
- `runtime.json` con `--extended-diagnostics` (`oot3d_native_a32_window.cpp:5769-5790`) **ha** `texture_copy_bytes` e `after_draw_submission_id`, ma `display_transfer_events` è una finestra mobile di 64 eventi (`5770-5772`): non è garanzia di contenere la copia.
- Gli eventi `draw` del trace (`oot3d_native_pica_semantic_trace.cpp:426-458`) portano `slot`, `physical_address`, `width`, `height`, `format`, `type`, `wrap_s/t`, `min/mag/mip_linear`, `lod_bias_raw`, `min_mip_level`, `max_mip_level`, `mip_level_count`, `byte_count`, `content_id`, `resource_content_id`: l'overlap con la destinazione si calcola lì.

Questi file e campi dimostrano bypass possibili e strumenti disponibili. NON dimostrano che il consumer del flashback li eserciti sull'output raw: questo resta un esperimento da eseguire sul modulo tradotto realmente caricato.

### Passi

I comandi di questa fase usano le variabili PowerShell definite in testa alla sezione 7 (`$install`, `$bin`, `$build`, `$ninja`, `$diag`, `$timeline`, `$trigger`, `$overlap`): eseguirle prima, oppure sostituire i percorsi equivalenti.

**D1 — stabilire quali copie raw esistono, poi correlare producer e consumer.**

Conseguenza dei campi verificati sopra: senza `texture_copy_bytes` nel trace la domanda “questo run contiene copie raw?” è indecidibile dal solo trace. Un campione `sheik-native-trigger` di 3400 frame in cui non si identificano copie raw **non prova** che non ce ne siano: prova solo che il canale usato non sa distinguerle. La verifica deve quindi prima rendere identificabili le copie, poi correlare.

**D1a — decidere se il run contiene copie raw, senza modificare sorgenti.** Due run indipendenti e complementari, sullo stesso checkpoint/timeline della baseline:

```powershell
# (1) runtime.json: display_transfer_events porta texture_copy_bytes, ma è finestra di 64 eventi.
python tools/triaevum_release/probe_renderer.py $install "$bin/TriAevum.exe" "$diag/gpucopy-d1-probe" --native-fidelity --frames 5000 --seconds 400 --no-captures --extended-diagnostics --load-state $trigger --input-timeline $timeline
Select-String -Path "$diag/gpucopy-d1-probe/runtime.json" -Pattern '"texture_copy_bytes": [1-9]'
# (2) stesso scenario con il checkpoint di overlap, già usato dal probe UI.
python I:/_triaevum_work/ui_profile_probe.py topscreen "$diag/gpucopy-d1-overlap" 1200 100
python tools/triaevum_release/probe_renderer.py $install "$bin/TriAevum.exe" "$diag/gpucopy-d1-overlap-json" --native-fidelity --frames 3500 --seconds 400 --no-captures --extended-diagnostics --load-state $overlap --input-timeline $timeline
Select-String -Path "$diag/gpucopy-d1-overlap-json/runtime.json" -Pattern '"texture_copy_bytes": [1-9]'
```

Nessun hit = **inconcludente** (la copia può cadere fuori dalla finestra di 64 eventi), non “nessuna copia”. Un hit dà un `CompletionId`/`after_draw_submission_id` concreto da inseguire. Ripetere su entrambi i checkpoint e su budget di frame che coprano la sequenza di rivelazione, non solo l'inizio.

**D1b — rendere il trace autosufficiente (un campo, writer esistente).** Aggiungere `{"texture_copy_bytes", transfer.Transfer.TextureCopyBytes}` a `RecordDisplayTransfer` (`oot3d_native_pica_semantic_trace.cpp:338-363`), accanto a `signal_interrupt` già presente. Niente nuovo sistema di log, niente finestra: il trace JSONL è già storia completa del run e questa è la sola forma che rende ricostruibile la storia delle copie su tutti i frame. Dopo questo, un singolo `movie_trace.py` sul flashback è sufficiente:

```powershell
python I:/_triaevum_work/movie_trace.py "$diag/gpucopy-d1-trace" $trigger $timeline 5000 60 1280 720
# poi, sull'output JSONL, elencare le copie raw e correlarle ai draw
Select-String -Path "$diag/gpucopy-d1-trace/pica_trace.jsonl" -Pattern '"event": "display_transfer"'
```

D1b è una modifica diagnostica di un solo campo nel writer esistente: non altera semantica dei transfer, target, shader o ordine draw, e va mantenuta anche dopo il cutover perché è l'unico canale che ricostruisce la storia completa delle copie. È la sola modifica di sorgente ammessa prima delle fasi A-C, e non è un'eccezione per scena o asset: vale per ogni `display_transfer`.

**D1c — correlazione overlap (necessaria, non sufficiente).** Per ogni `display_transfer` con `texture_copy_bytes != 0`: calcolare l'intervallo destinazione `[output_physical_address, +size)` e i suoi gap come in `gfx_vulkan_pica_texture_copy.cpp:57-67`; cercare gli eventi `draw` successivi al confine `after_draw_submission_id` con almeno un texture binding (`physical_address`, `byte_count`) sovrapposto; risalire al draw corrispondente in `PicaSceneFrame::Record` e alla sua risoluzione nel backend. Includere nel conteggio le copie concatenate (output di una copia riletto da una copia successiva) e i consumer non-texture.

### Criterio che distingue “consumer solo dal sampler GPU” da “lettura host della destinazione”

La classificazione è per intervallo e per confine, e si decide su tre categorie di accesso alla destinazione:

1. **Binding texture del renderer.** Il draw che campiona l'intervallo. Oggi è servito dai byte CPU di `CaptureTextureResources`; dopo B è servito dalla versione GPU-owned. Un overlap trovato in D1c è condizione necessaria, **mai** sufficiente: non dice nulla su come quei byte sono raggiunti.
2. **Materializzazione esplicita.** Snapshot/savestate/capture (B) e readback diagnosticato. Ammessa, contata e attribuita come tale.
3. **Accesso host del consumer tradotto.** Qualunque lettura o scrittura della destinazione fuori dai due precedenti: `ReadFast`/`ReadBytes`/`GetReadPointer`/`GetWritePointer` (`oot3d_native_a32_memory.h:88-125,158-174,409-445`), regioni fast/unobserved di `oot3d_native_a32_memory_region.h:39-71,151-172`, copy bulk/VFP (`whole_aot_cpp.py:1178-1183,1231-1262`), calcolo di hash/replacement sui byte.

- **GPU-sampler-only** ⇔ la categoria 3 è vuota e resta vuota sotto la prova dinamica D3 sull'**intero** range (con PC/kind registrati) e sotto la prova avversaria D4. Allora la copia può restare GPU-owned e il sampling GPU è l'unico consumatore.
- **host-read** ⇔ almeno un accesso in categoria 3, con il punto d'ingresso registrato. Quel punto decide on-demand se è intercettabile, altrimenti eager al confine realmente host-observable.
- L'assenza di righe nel trace PICA non è **mai** evidenza di assenza di accesso host: gli eventi `draw`/`display_transfer` provano solo cosa fa la GPU. La discriminazione viene da D3 + D4, non dal trace.
- Un watchdog da quattro byte, un `--pica-semantic-trace` senza hit o una singola run visivamente corretta non classificano un intervallo. La copertura dev'essere su tutti i byte dell'intervallo e su tutti i percorsi host raggiungibili.

**D2 — seguire il modulo realmente eseguito.** `invocation.json` del probe identifica title-plugin/process-manifest/shader pack; registrare hash e manifest. Usare il manifest di generazione per risalire a owner function/shard e ai PC delle operazioni che possono raggiungere gli intervalli di output. Ispezionare i load/store e gli helper host di quei consumer, inclusi `ReadFast`, puntatori restituiti prima della copia, regioni cached, VFP/bulk copy e callback native. Trace PICA prova accessi GPU, non l'assenza di letture CPU. Non sostituire la DLL privata con un diverso percorso legacy e chiamarlo oracolo.

Strumenti già presenti: `generate_whole_aot_cpp.py:19-58` (`--program`, `--selection`, `--code`, `--output`, `--shards`, `--shard-strategy`), `generate_whole_aot_guest_map.py:219-243` (ownership shard), `audit_whole_aot_product.py:376-385`. Non generare una selezione privata e pubblicarla nel repository.

**D3 — prova dinamica degli accessi host.** Nella build AOT/shard di sviluppo corrispondente al modulo, usare debugger/data breakpoint sui range host risolti e/o strumentazione diagnostica temporanea nelle primitive/lowering già elencate per registrare PC, range, read/write e producer version. Coprire TUTTI i percorsi raggiungibili, non solo `ReadBytes` né fingerprint scritture. Un watchpoint di quattro byte o un trace senza hit non prova l'assenza di accessi sull'intero range. Ripetere sui due checkpoint e su reload. `NativeA32UnobservedMemory` e i puntatori conservati devono comparire nella review della copertura.

Decisione: on-demand è accettabile solo se ogni accesso host potenziale al range GPU-owned attraversa il punto che materializza/invalida, o se una prova statica sul consumer chiude tutti i bypass; altrimenti eager al confine realmente host-observable. Se né intercept né confine eager sono provati, D fallisce e il cutover non si abilita. Non dedurre GPU-only dal successo visivo di una singola run.

**D4 — prova avversaria e ABI.** In modalità diagnostica privata lasciare deliberatamente non aggiornato lo shadow RAM solo nei range che il nuovo contratto dichiara GPU-owned, mantenendo il contenuto corretto sulla GPU; eseguire il consumer tradotto e verificare che accessi CPU materializzino prima di leggere. Confrontare valori/continuazione guest, PPF, framebuffer e fault; ripetere con CPU write parziale e puntatore preacquisito. Questa diagnostica non è una modifica di fixture per far passare i test. Poi rimuoverla e ripetere l'oracolo reale. Se cambiano layout/inlined access di NativeA32Memory, ricompilare il modulo AOT coerente e verificare compatibilità ABI/caricamento: il vecchio binario precompilato non acquisisce nuovi intercept aggiungendo solo un metodo host.

### Comandi/gate D

C3 per trace; C7 per build/audit AOT; C6 per reload; debugger supportato dal sistema (data breakpoint/stack su processo TriAevum) per la prova diretta. I comandi di D1a/D1b sono inline nella fase D. I PC e i path del manifest/code privati vanno risolti dal modulo reale, non indovinati nel piano.

Output obbligatorio dell'esperimento, fuori repo: tabella producer/range -> consumer PC/shard -> tipo accesso -> punto di intercettazione -> prima visibilità CPU -> evidenza. Risultato accettabile: tutti gli accessi classificati e prova on-demand/eager completata, nessun interpreter fallback, zero letture stale. Fino ad allora la decisione è **aperta**, non “consumer sicuramente solo GPU”.

## 7. Comandi e matrice di verifica

Tutti i comandi seguenti sono da eseguire durante l'implementazione, non risultati già ottenuti da questo documento. PowerShell, dalla radice checkout; usare directory output nuove. Gli harness privati `movie_trace.py`/`ui_profile_probe.py` eliminano l'output se esiste: mai puntarli alle baseline o all'installazione. Il probe pubblico usa invece `exist_ok=False`.

```powershell
$bin = 'I:/TriAevum-public/ui-dependencies/bin'
$build = 'J:/TriAevum-verify-20260910/runtime'
$ninja = 'C:/Users/xander/.local/bin/ninja.exe'
$install = 'I:/TriAevum-public/releases/v0.6.0-alpha.3b/qualification-windows'
$diag = 'I:/_triaevum_work'
$timeline = 'tools/oot3d/native_game_runtime/input_timelines/native_cutscene_dialogue_advance.json'
$trigger = 'J:/TriAevum-diagnostics/issues45-46/sheik-native-trigger/checkpoint.oot3dsav'
$overlap = 'J:/TriAevum-diagnostics/issues45-46/sheik-copy-overlap-save/checkpoint.oot3dsav'
```

### C1 — suite esistenti e loro copertura reale

I cinque eseguibili principali sono presenti nella directory bin verificata. Target transfer/submission/bridge/visual definiti in `CMakeLists.txt:1835-1841,1868-1871,2253-2258`.

```powershell
& $ninja -C $build oot3d_native_pica_transfer_tests oot3d_native_pica_submission_tests oot3d_native_pica_vulkan_bridge_tests oot3d_native_pica_visual_savestate_tests oot3d_graphics_foundation_tests
& "$bin/oot3d_native_pica_transfer_tests.exe"
& "$bin/oot3d_native_pica_submission_tests.exe"
& "$bin/oot3d_native_pica_vulkan_bridge_tests.exe"
& "$bin/oot3d_native_pica_visual_savestate_tests.exe"
& "$bin/oot3d_graphics_foundation_tests.exe"
```

Ogni comando deve avere exit 0; interrompere al primo errore, non lasciare che l'ultimo exit nasconda un fallimento precedente. Main esegue build/suite una volta dopo integrazione, non durante edit concorrenti. Ninja usa l'albero configurato; non invocare CMake dalla bash.

| Suite | Cosa prova oggi | Cosa NON prova da sola |
|---|---|---|
| transfer | `oot3d_native_pica_transfer_tests.cpp:33-61,65-80,117-203`: oracle Morton/channel/scale, gap, size allineata, wrap/unmapped senza scrittura parziale, combinazioni width/gap/length e region planner | Barriere e contenuto prodotto dal kernel GPU |
| submission | `...submission_tests.cpp:132-140,148-155,342-350,389-393`: snapshot/queue, mip, completion order, raw command conservato | Guest host-pointer coherence della build AOT |
| Vulkan bridge | `...vulkan_bridge_tests.cpp:335-460`: ordine draw/transfer, dependency flush, nessun scanout inventato, raw width/gap conservati | È RecordingBackend, non prova Vulkan hardware o PPF relay reale |
| visual-savestate | `...visual_savestate_tests.cpp:224-255`: round-trip raw, pending completion restaurato e drenato una volta | In-flight GPU nuovo e visibilità byte dopo readback differito: richiedono nuova prova mirata |
| foundation | `runtime/three_ds_recomp/tests/oot3d_graphics_foundation_tests.cpp:106-181`, in particolare `Oot3dPicaRasterCanvas.ScreenSpaceContentCoversAndPlacedUiKeepsNativeFit:140`: UI/Screen layout e contratti renderer | Non sostituisce tutta la suite scene-frame né prova la classificazione live |

Eseguibili aggiuntivi presenti da costruire/eseguire quando il loro contratto è toccato: `oot3d_native_pica_vulkan_plan_tests`, `oot3d_native_pica_semantic_trace_tests`, `oot3d_native_a32_savestate_tests`, `oot3d_native_pica_texture_tests`, `oot3d_pica_shader_pipeline_cache_tests`; stesso comando Ninja + `$bin/<target>.exe`. Non cambiare shader fixture o expected capture per “ripinnare” un risultato differente.

`oot3d_pica_scene_frame_tests` NON è gate eseguibile in questo albero: link noto mancante `Oot3d::Renderer::AzaharCityHash64`; non riprodurlo e non includerne la riparazione in questa migrazione. Foundation copre il canvas; bridge/submission/visual coprono i contratti elencati; per `PicaSceneFrame` live usare diagnostica del gioco e capture C3/C5. Dichiarare questa lacuna nel report finale, non affermare copertura equivalente.

### C2 — prova diretta GPU contro oracle

La suite transfer è CPU, quindi durante A serve una prova GPU del vero `SubmitNativePicaTextureCopy` seguita da sampling e materializzazione diagnostica. Usare un harness mirato esterno al repository costruito contro il backend reale/NRI e gli oracle header esistenti, oppure aggiungere un caso comportamentale a una fixture GPU esistente solo dopo averne verificato il runtime. Non introdurre un framework nuovo né un mock che ripete i propri input.

Il comando preciso di questo harness non esiste oggi: il gate non è falsamente soddisfatto da C1. In alternativa immediatamente eseguibile, usare C3 e C6 per produrre snapshot owner pre/post e confrontare gli span contro `BuildOot3dPicaTextureCopyPlan`/`EncodePicaFramebuffer` con un verificatore privato. Prima di accettare il kernel in tutti i formati, il test diretto deve coprire formati 0..4 dichiarati supportati, texel parziali se supportati, gap inalterati, off-base, scale diverse, layout incompatibile rifiutato, due versioni e CPU overwrite. I casi non coperti non si dichiarano supportati sulla base del solo flashback.

Per un check permanente giustificato da queste nuove e reali ambiguità, target proposto **nuovo** `oot3d_pica_gpu_texture_copy_tests` (file **nuovo** `runtime/three_ds_recomp/tests/oot3d_pica_gpu_texture_copy_tests.cpp`, wiring nel CMake dei test renderer esistente):

```powershell
# Solo dopo aver introdotto il target mirato durante A, non disponibile oggi:
& $ninja -C $build oot3d_pica_gpu_texture_copy_tests
& "$bin/oot3d_pica_gpu_texture_copy_tests.exe"
```

La fixture deve chiamare backend/copy/sampling reali; test readback esplicito consentito, hot-path readback vietato. Mantiene l'oracle CPU invariato. Se il contesto non permette tale fixture, il rilascio resta bloccato finché la prova equivalente non è eseguibile, anziché assegnare alla suite CPU una copertura GPU inesistente.

### C3 — flashback e trace strutturale, due harness riusati

Il wrapper indicato nel contesto `I:/_triaevum_work/probe_renderer.py` non risulta presente: usare direttamente l'implementazione esistente `tools/triaevum_release/probe_renderer.py` (CLI a 38-69, isolamento/capture a 166-193). Il secondo harness `I:/_triaevum_work/movie_trace.py` esiste: CLI a 4 e 18-40, abilita già `--pica-semantic-trace`. Per l'identificazione delle copie raw questo trace richiede il campo aggiunto in D1b; senza di esso usare il canale `display_transfer_events` con la cautela della finestra di 64 eventi.

```powershell
python tools/triaevum_release/probe_renderer.py $install "$bin/TriAevum.exe" "$diag/gpucopy-after-flashback" --native-fidelity --frames 5000 --seconds 400 --capture-interval 300 --extended-diagnostics --load-state $trigger --input-timeline $timeline
python I:/_triaevum_work/movie_trace.py "$diag/gpucopy-after-trace" $trigger $timeline 3500 60 1280 720
python I:/_triaevum_work/compare_captures.py I:/_triaevum_work/movie5 "$diag/gpucopy-after-flashback"
```

Usare una baseline A/B creata con medesimo harness, cache/seed/config/timing/dimensioni e stato; `movie5` è anche riferimento storico misurato, non un permesso a ignorare differenze tra eseguibili/config. `movie_trace.py` e probe hanno cadenze screenshot diverse: non confrontare file per indice tra questi due output. La sequenza trace include frame 3120/3420 a intervalli 60. L'harness di confronto (`compare_captures.py:17-28`) zip-a le liste e usa soglia per pixel >8: non prova byte identity né segnala automaticamente lunghezze diverse. Controllare nomi/numero di frame e digest separatamente.

Per profili UI previsti dal prodotto riusare anche:

```powershell
python I:/_triaevum_work/ui_profile_probe.py topscreen "$diag/gpucopy-after-ui" 200 100
```

Quel helper usa già il checkpoint overlap (`ui_profile_probe.py:15,29`) ed è un controllo aggiuntivo, non un test di cold boot.

### C4 — prestazioni e diagnostica, senza spostare il costo

Baseline verificata leggendo `I:/_triaevum_work/movie5/runtime.json` e `invocation.json`: 5000 presentazioni, native30/fixed delta, Authentic, 1280x720, capture sincrone ogni 300, extended diagnostics, stesso checkpoint trigger/timeline.

`visual_interpolation.execution_scheduler`:

- `snapshot_transfer_seconds = 27.699280500000164` (5.5398561 ms per frame presentato);
- `present_transfer_seconds = 0.4905742999999999`;
- `draw_submission_seconds = 27.663887100006022`;
- `execute_seconds = 56.180080899999936`;
- `captured_display_transfers = 9278`, `display_snapshots_executed = 9278`, `dependency_flushes = 692`, `duplicate_draw_attempts = 0`.

Il valore ~25.65 s per 7534 frame è una run storica diversa (`docs/TRIAEVUM_ISSUES_45_46.md:239-248`), non confrontare direttamente ms/frame di due scenari/config diverse. `snapshot_transfer_seconds` include anche transfer non raw: timer attuale a `oot3d_native_pica_presentation_scheduler.cpp:223-226`, JSON a `oot3d_native_a32_window.cpp:7578-7581`.

```powershell
$r = Get-Content "$diag/gpucopy-after-flashback/runtime.json" -Raw | ConvertFrom-Json
$r.visual_interpolation.execution_scheduler | Format-List
python tools/triaevum_release/probe_renderer.py $install "$bin/TriAevum.exe" "$diag/gpucopy-after-timing" --native-fidelity --frames 5000 --seconds 400 --no-captures --extended-diagnostics --load-state $trigger --input-timeline $timeline
```

Eseguire baseline e candidato per almeno tre run abbinate, stessa politica cold/warm cache, riportare mediana e dispersione. Gate: mediana `snapshot_transfer_seconds` e costo totale copia+materializzazione inferiori alla baseline oltre la dispersione misurata; zero regressione sistematica di execute/guest/frame pacing, stessi eventi/progressione nativa. Non imporre un numero di speedup inventato. Le run con capture qualificano le immagini; quelle senza capture qualificano le prestazioni. Non scambiare tempi submission CPU con durata GPU.

Per attribuzione aggiungere pochi contatori alla diagnostica ESISTENTE (`include/fast/backends/oot3d_vulkan_diagnostics.h:157,571-634` e relativa implementazione) oppure al timing scheduler: **nuovi**, non oggi presenti: copie raw/byte, raw-submit CPU seconds, attese token seconds, materialization bytes/count/cause, raw-copy readback count, alias GPU hits, live/pinned/retired bytes, PPF pending/delivered/duplicate per id. Non loggare ogni pixel, non aggiungere una piattaforma telemetry. Riutilizzare `OOT3D_VULKAN_DIAGNOSTICS_PATH` impostato dal probe a 186-187. In diagnostica distinguere esplicitamente `ordered-copy`, `guest-read`, `savestate/capture` per i readback.

Controllo negativo: nel metodo copia e nel suo callgraph caldo non devono comparire `CaptureNativePicaImageBytes`, `vkQueueWaitIdle`, `vkDeviceWaitIdle` o `WaitForAllPresents`; verificare via ricerca codice e contatori/breakpoint sul percorso reale. La loro presenza nelle snapshot esplicite è corretta.

### C5 — inerzia boot/file-select byte-exact

Le invocation esistenti `I:/_triaevum_work/screen_boot/invocation.json` e `screen_fileselect/invocation.json` sono state lette: rispettivamente 420/1200 frame, capture 60/200, native fidelity, 1280x720; file-select usa `title_to_file_select.json`.

```powershell
python tools/triaevum_release/probe_renderer.py $install "$bin/TriAevum.exe" "$diag/gpucopy-after-boot" --native-fidelity --frames 420 --seconds 120 --capture-interval 60
python tools/triaevum_release/probe_renderer.py $install "$bin/TriAevum.exe" "$diag/gpucopy-after-files" --native-fidelity --frames 1200 --seconds 150 --capture-interval 200 --input-timeline tools/oot3d/native_game_runtime/input_timelines/title_to_file_select.json
```

Confrontare con le baseline Screen già qualificate, non con quelle pre-correzione; verificare prima identità del set di nomi e poi byte/digest di ogni capture, non soltanto immagini visivamente simili. Check eseguibile senza nuovi asset:

```powershell
$a = Get-ChildItem 'I:/_triaevum_work/screen_boot/framebuffer_*.bmp' | Sort-Object Name
$b = Get-ChildItem "$diag/gpucopy-after-boot/framebuffer_*.bmp" | Sort-Object Name
if ($a.Count -eq 0 -or $a.Count -ne $b.Count) { throw 'capture set mismatch' }
for ($i=0; $i -lt $a.Count; $i++) {
  if ($a[$i].Name -ne $b[$i].Name -or (Get-FileHash $a[$i].FullName).Hash -ne (Get-FileHash $b[$i].FullName).Hash) { throw "capture mismatch: $($a[$i].Name)" }
}
```

Ripetere per file-select. Per flashback, lo stesso controllo exact deve passare per il cutover semanticamente equivalente; la misura di desaturazione di `compare_captures.py` è supplementare e deve mantenere l'effetto sull'intera larghezza, non sostituire l'oracle byte.

### C6 — restore e completamento guest

```powershell
& $ninja -C $build oot3d_native_a32_savestate_tests
& "$bin/oot3d_native_a32_savestate_tests.exe"
python tools/triaevum_release/probe_renderer.py $install "$bin/TriAevum.exe" "$diag/gpucopy-after-overlap" --native-fidelity --frames 3500 --seconds 400 --capture-interval 60 --extended-diagnostics --load-state $overlap --input-timeline $timeline
python tools/triaevum_release/probe_renderer.py $install "$bin/TriAevum.exe" "$diag/gpucopy-after-save" --native-fidelity --frames 400 --seconds 120 --capture-interval 60 --extended-diagnostics --load-state $overlap --input-timeline $timeline --save-state-frame 120
```

Poi nuovo processo con `--load-state "$diag/gpucopy-after-save/checkpoint.oot3dsav"`, stessa continuazione deterministica e confronto a pari guest frame. Il frame 120 è solo smoke di round-trip: per la nuova transizione in-flight salvare/ripristinare anche esattamente ai confini pending/recorded/completed/delivered tramite test mirati e trace. Contatori aggregati e un checkpoint arbitrario non provano exactly-once.

### C7 — AOT/shard e audit prodotto

Il build helper esistente espone `-Target`, `-BuildDirectory`, `-PreflightOnly`, `-Configure` (`tools/oot3d/build_fast_dev.ps1:1-12`); genera A32 a 79-98 e whole-AOT solo se la configurazione lo richiede a 100-115. Prima ispezionare cache/manifest del modulo reale; non assumere che quel tree generi automaticamente la DLL privata installata.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/oot3d/build_fast_dev.ps1 -Target oot3d_native_game -BuildDirectory J:/TriAevum-verify-20260910/runtime -PreflightOnly
# Dopo aver risolto gli input privati e la corrispondenza del modulo:
powershell -NoProfile -ExecutionPolicy Bypass -File tools/oot3d/build_fast_dev.ps1 -Target oot3d_native_game -BuildDirectory J:/TriAevum-verify-20260910/runtime
python tools/oot3d/native_a32_runtime/audit_whole_aot_product.py --help
```

Audit effettivo: `python .../audit_whole_aot_product.py --program <aot_program.json-del-modulo> --code <code.bin-privato> --selection <selection-del-modulo> --generated-manifest <manifest-shard-del-modulo> --json`, aggiungendo `--manifest` se diverso dal default e `--exheader` quando richiesto dagli input. I placeholder sono prerequisiti privati da risolvere con D2, non file da creare o assumere. Atteso audit coerente di owner/shard/hash e modulo caricato corrispondente; suite A32 dipendenti dal registry eseguibili soltanto nell'albero completo. La documentazione segnala l'assenza di A32 generated inputs nel tree precedente (`TRIAEVUM_ALPHA2C_TO_ALPHA3_RECONSTRUCTION.md` §8); non chiamare verde un test escluso per questo motivo.

## 8. Rischi, rollback e gate di rilascio

| Rischio | Contromisura verificabile |
|---|---|
| Copia raw trasformata in blit filtrato/quantizzazione saltata | Oracle bit-exact C1+C2, source scale/formati, nessuna rotazione o conversione colore opzionale |
| Guest/AOT legge shadow RAM stale o usa pointer cached | D obbligatoria, copertura fast/region/pointer e prova avversaria; ABI/module rebuild se necessario |
| Same-value CPU write / partial overlap / layout alternativo | Versioni e ownership per intervallo; non solo equality `Before` o token globale; test prima/dopo e RMW |
| Copy-on-write assente, vecchio snapshot muta | Pin versione/producer e retirement per fence + riferimenti replay; C2 e restore |
| View/sample dopo free, descriptor modificato in-flight | Borrowing esplicito, retirement differito, versioni bounded; validazione Vulkan/NRI e lunga sequenza |
| PPF duplicato/anticipato/perso in restore | Identità completion+epoch, `SignalInterrupt` end-to-end, stato persistito; C1+C6 |
| Nuova via CPU mascherata | Contatori per causa, run senza capture; nessun fallback dal GPU owner al CPU executor |
| Costi spostati da submit a read/snapshot o shader compile | C4 include attese/materializzazione/execute, cache cold/warm e shader canonical miss; snapshot misurate separatamente |
| Layout, namespace e resize/MSAA invalidation | Epoch e layout verificati; unsupported esplicito; restore/reset riaggancia versioni senza riusare handle vecchi |
| Snapshot eliminate per “zero readback” | Gate B: capture texture/color/presentation e savestate restano disponibili e coerenti |

Rollback operativo: conservare prima del cutover binario/config/manifest e risultati della baseline fuori repo; integrare modifiche A/B/C/D in patch/commit separati e isolati dalle correzioni già presenti. Se un gate fallisce, non distribuire: ripristinare soltanto la migrazione alla baseline completa e misurata, preservando Screen e le altre correzioni. Non usare reset distruttivi sul checkout non committato, non offrire toggle runtime per tornare al CPU per singola scena/transfer. Se cambia il formato savestate, rollback riusa checkpoint baseline compatibili; non carica uno schema nuovo ignorandone i campi.

Gate finale obbligatorio: A byte-equivalence GPU + B binding/coerenza + C PPF/materializzazione + D consumer AOT classificato; suite pertinenti verdi (limiti preesistenti dichiarati); capture framebuffer byte-exact dei controlli e flashback corretto; nessun errore Vulkan/NRI nuovo; metriche prima/dopo abbinate con riduzione misurata e memoria bounded. Un quadro visivo plausibile o una suite CPU verde non basta. Nessuna affermazione prestazionale senza run no-capture né “tutte le suite verdi” includendo il target scene-frame non linkabile.

## 9. Fuori ambito e cambiamenti protetti

Non toccare né riverificare come nuove correzioni: #53 `atomic_write_bytes`, #51 F1 event toggle, #49/#45 persistenza/gate PPF già corretti, root dati AppImage/launcher/AppRun, SDL Steam virtual controller, classificazione `Screen`/`ResolvePicaRasterCanvas`/`HasSceneDrawOnColorTarget` e il relativo lavoro non committato. Le eventuali integrazioni in file condivisi devono essere hunk stretti a valle delle modifiche esistenti.

Non introdurre eccezioni per Zelda/flashback, formato o indirizzi del pacchetto osservato; il packet della riproduzione è una fixture/oracolo, non una condizione runtime. Non cambiare shader/state canonici, draw order, target o interpretazione nativa dei transfer. Nessun batch separato di ottimizzazione AOT/cache/renderer, nessun framework resource-manager/telemetry nuovo, nessuna nuova dipendenza. Non risolvere in questa patch il simbolo AzaharCityHash64 mancante.

ROM, code.bin, asset, save, capture, moduli derivati, generated shard e output diagnostici restano nei percorsi privati. Questo artefatto è `local://gpu-copy-path-plan.md`, non un file del repository.

## 10. Stato del presente deliverable

Verificato per lettura: file/simboli/riferimenti citati, separazione hot copy vs snapshot idle vs sampling live, disponibilità dei cinque eseguibili principali e harness effettivi, parametri CLI, baseline JSON e invocation, campi esatti dei due canali diagnostici (trace `display_transfer` senza `texture_copy_bytes` ma senza finestre; `display_transfer_events` con il campo ma finestra di 64) e il trace come storia completa del run (`WriteEvent:676-679`, flush a 669). Non sono stati eseguiti build, suite o nuove run del gioco; non si dichiara implementazione o miglioramento misurato.

Decisioni ancora da risolvere con esperimento: presenza/ubicazione delle copie raw nei run di riferimento (D1a/D1b); consumer AOT host/GPU effettivo (D1c, D2, D3); intercettabilità completa dei pointer/fast path e impatto ABI; scelta definitiva on-demand vs eager necessario; copertura GPU effettiva di formati/layout/MSAA. Sono gate pianificati, non assunzioni silenziose.

## 8. Stato di esecuzione (aggiornato)

- **D1a/D1b — fatto.** Il trace registra `texture_copy_bytes`
  (`oot3d_native_pica_semantic_trace.cpp`, `RecordDisplayTransfer`), con test che
  copre il caso display-transfer (0) e il caso copia raw (nonzero). Il run di
  rivelazione (3500 frame da `sheik-native-trigger` + timeline dialogo) contiene
  **514 copie raw**, tutte 768.000 byte, destinazione `0x183E3300..0x184AAF00`,
  dal presentation frame 2473 ogni due frame, `SignalInterrupt` vero.
- **D1c — fatto.** Il consumatore è un binding di sampler, uno per copia, nello
  stesso frame: draw `ui/ui` (`source_pc` zero) che campiona una texture da 1 MiB
  a `0x183AB300` sovrapposta alla destinazione. Overlap trovato = condizione
  necessaria, non sufficiente: resta da provare che non esista un accesso host.
- **Fase A, sottoinsieme sicuro — fatto e misurato.** `SubmitNativePicaTextureCopy`
  non usa piu' `vkQueueWaitIdle`: la submission della copia attende il proprio
  fence dedicato (`mRawCopyFence`), che per ordine di submission sulla stessa
  queue implica comunque il ritiro di tutto il lavoro precedente. `WaitForAllPresents`
  e' ancora presente: la sua rimozione appartiene al passo 7 del piano (ritiro
  differito delle texture), che richiede il registro di versioni non ancora
  costruito. Misura nella finestra delle copie (3500 frame, 513 flush):
  `snapshot_transfer_seconds` 24,312 s -> 20,647 s (6,95 -> 5,90 ms/frame),
  `execute_seconds` 46,742 s -> 43,745 s, capture **byte-identiche** su 12 frame.
- **Attribuzione del costo copia — fatto.** La copia ordina i propri costi per
  causa (`TRIAEVUM_RAW_COPY`, stampato una volta allo shutdown accanto agli altri
  receipt in `gfx_vulkan.cpp`; contatori `RawCopyTiming` in `gfx_vulkan.h`).
  Sulla finestra delle copie, prima degli interventi: 513 copie, 25.650 span,
  566 texture invalidate, `readback` 16.214 ms su 17.238 totali (**94%**),
  `encode` 568 ms, `invalidate` 290 ms, `span` 125 ms, `commit` 41 ms. Il costo
  non era quindi l'encode PICA ne' il writeback, ma la readback sincrona.
- **Readback in-band e staging cached — fatto e misurato.** Due difetti reali,
  non ipotizzati: (1) la readback apriva un comando immediato proprio
  (`vkQueueSubmit` + `vkQueueWaitIdle`), una seconda submission e un secondo
  svuotamento di queue per ogni copia; (2) `CreateBuffer(... HOST_VISIBLE |
  HOST_COHERENT ...)` sceglieva il primo tipo di memoria host-visible, che su
  questa macchina e' write-combined, e la lettura CPU da quella memoria rendeva
  ~289 MB/s. Ora la copia legge la sorgente **dentro la submission che gia'
  attende** (barriera, `vkCmdCopyImageToBuffer`, barriera di ritorno) e usa uno
  staging persistente mappato scelto da `FindReadbackMemoryType`, che preferisce
  `HOST_CACHED` e invalida esplicitamente quando il tipo non e' coerente. Byte
  consegnati all'encoder identici. Costo per copia: 33,6 ms -> **4,36 ms**
  (`readback` 25,5 -> 1,98, `encode` 1,13, `invalidate` 0,92, `span` 0,24,
  `commit` 0,08). Finestra copie: `snapshot_transfer_seconds` 5,90 -> **1,83
  ms/frame**, `execute_seconds` 12,50 -> **7,84 ms/frame**, due run appaiate
  (1,832 / 1,825 ms/frame, dispersione 0,007). Capture **byte-identiche** su 12
  frame; boot 5/5 hash-exact; file-select 6/6 identico alle tre baseline
  concordanti `n_/dom_/cover_fileselect` (`screen_fileselect` e' di un eseguibile
  diverso, come avverte la sezione C5); 10 suite PICA verdi.
- **Riferimento prestazionale.** Gameplay nella stessa scena (2000 frame):
  `captured_display_transfers` 3990 ma **`dependency_flushes` 0 e zero copie
  raw**: il percorso caldo qui studiato non e' esercitato dal gameplay, quindi
  0,068 ms/frame e' il costo delle snapshot di display-transfer, non un termine
  di paragone per la copia. Il paragone corretto e' la finestra copie contro se
  stessa: 6,95 -> 1,83 ms/frame. Il run gameplay con il nuovo binario resta
  invariato (0 copie, `snapshot` 0,051 ms/frame, `execute` 5,05 ms/frame).
- **Confine misurato.** Dei 4,36 ms/copia residui, ~1,98 sono la lettura della
  sorgente (~7,4 MB per copia), ~1,13 l'encode PICA su CPU, ~0,92 il ritiro
  delle texture sovrapposte: e' il modello CPU-encode, non overhead di
  sincronizzazione. Non e' riducibile in Fase A/C: serve la Fase B (binding
  GPU-owned) perche' il sampler consumi una risorsa versionata invece dei byte
  riportati in guest, e l'encode possa restare su GPU. Finche' D3/D4 non provano
  che il consumer tradotto non legge la destinazione via host pointer, il
  writeback guest-visibile resta e questo pavimento resta.
- **Prossimo.** A reale (copia/encode su GPU verso risorsa versionata), B
  (binding GPU-owned, risoluzione versionata prima di hash/decode), C
  (materializzazione solo se serve) subordinata a D3/D4: finche' non e' provato
  che il consumer tradotto non legge la destinazione via host pointer, il
  writeback guest-visibile resta e il giro CPU non si puo' togliere.
- **Controllo negativo C4, stato attuale (dopo la correzione readback).** Nel
  metodo copia non compaiono piu' `CaptureNativePicaImageBytes`,
  `vkQueueWaitIdle` ne' `vkDeviceWaitIdle`: la copia legge la sorgente in-band
  sulla propria submission e attende il proprio fence. `WaitForAllPresents`
  resta, e resta il termine del passo 7 (ritiro differito delle texture), che
  richiede il registro di versioni non ancora costruito; la sua attesa non e'
  pero' il termine dominante: i 4,36 ms per copia si spiegano interamente con le
  fasi misurate (`readback` 1,98 + `encode` 1,13 + `invalidate` 0,92 + `span`
  0,24 + `commit` 0,08), e la finestra copie sta a 1,83-1,92 ms/frame.
- **Prerequisito D non ancora eseguito, per scelta.** L'esperimento D3/D4
  (guardia sulle letture host della destinazione) non e' stato avviato: la
  risposta serve a C, che non e' iniziata, e senza C l'evidenza resterebbe
  inutilizzata. Va eseguito quando C entra in lavorazione, con le due avvertenze
  del §6: `ReadFast` e' inline e non virtuale, quindi agganciare solo
  `GetReadPointer` non intercetta il percorso reale, e il modulo titolo e'
  precompilato, quindi la guardia deve agire sul mapping/percorso e non su
  codice ricompilato.
- **Difetti di confine e di build trovati in questo passaggio e corretti** (non
  erano difetti della copia): il gate `verify_renderer_3ds_cross_title_boundary`
  era rosso su due file dello shared renderer, `oot3d_pica_scene_frame_tests`
  non era mai stato linkato, e lo smoke F1 linkava copie stale delle librerie.
  Dettaglio in `TRIAEVUM_ALPHA2C_TO_ALPHA3_RECONSTRUCTION.md` §3.4.
