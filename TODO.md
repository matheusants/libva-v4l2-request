---
# libva-v4l2-request — TODO & Status

## Estado atual (05/05/2026)

**Causa raiz da corrupção de P/B frames — MULTI-FATOR:**

### Kernel: `copied_timestamp` gate in `vb2_find_timestamp`
`refs/orange-pi-5.15-sun55iw3/drivers/media/common/videobuf2/videobuf2-v4l2.c:653`

O BSP T527 exige `q->bufs[i]->copied_timestamp == 1`, flag setada APENAS por
`v4l2_m2m_buf_copy_metadata()` durante o DQBUF (pós-decode). O campo
`copied_timestamp` NÃO pode ser setado durante o QBUF(CAPTURE) — tentar
fazer isso trava o pipeline do BSP T527.

**Solução:** bypass no lado cedrus via patch 0005 (substitui `vb2_find_timestamp`
por loop manual sobre `cap_q->bufs[j]->timestamp`, sem verificar `copied_timestamp`).

**Patch 0005:** `refs/patch_cedrus/0005-cedrus-fix-vb2_find_timestamp.patch`
Substitui `vb2_find_timestamp` por loop manual sobre `cap_q->bufs[j]->timestamp`
(bypassa `copied_timestamp`, usa campo correto sem wrapper `vb2_buf`).

### Userspace Bug 1 (incompleto): DPB slot reuse — STALE POINTER (`src/h264.c:586-640`)
`dpb_update()` pode chamar `dpb_find_oldest_unused_entry()` que retorna o slot
`reserved` para CurrPic (age=0 do memset) e reinsere uma referência nele.
`dpb_insert(CurrPic, output)` sobrescreve a referência → referência perdida.
**Fix parcial:** `output_idx` salvo antes de `dpb_update()`, ponteiro recuperado depois.
**Mas:** o slot reservado AINDA é roubado por `dpb_update` (Bug 4).

### Userspace Bug 2: Stale PRED_WEIGHTS (`src/v4l2_translation.c`)
`MEDIA_REQUEST_IOC_REINIT` é no-op no BSP T527. Se frame B envia PRED_WEIGHTS,
o residual persiste no request para o próximo frame P (que enviava condicional).
Kernel aplica ponderação errada do frame anterior.
**Fix:** Sempre enviar PRED_WEIGHTS (zerado quando não necessário) para overwrite.

### Userspace Bug 3: Timestamp collision (`src/picture.c:318`)
`gettimeofday()` resolução de µs. Decodes consecutivos podem gerar timestamps
idênticos → `vb2_find_timestamp` retorna primeiro match (buffer errado).
**Fix:** Contador monotônico adicionado ao `tv_usec` para garantir unicidade.

### Userspace Bug 4 (ROOT CAUSE real): DPB slot theft (`src/h264.c:128-145`)
`dpb_find_oldest_unused_entry()` NÃO verificava `entry->reserved`.
Após `dpb_clear_entry(output, true)`, o slot reservado tem `used=false, age=0`.
Quando todos os slots DPB estão ocupados e `dpb_update` precisa reinserir
uma referência, `dpb_find_oldest_unused_entry()` SEMPRE retorna o slot reservado
(age=0 é o mínimo possível). `dpb_update` coloca uma REFERÊNCIA lá.
Depois `dpb_insert(CurrPic, output, ts)` sobrescreve a referência com CurrPic.
**Efeito:** referência perdida silenciosamente → kernel busca timestamp da
referência → não encontra → luma_ptr=0 no SRAM → Y=0/pixels trocados.

**Fix:** Adicionar `!entry->reserved` ao `if` em `dpb_find_oldest_unused_entry()`:
```c
if (!entry->used && !entry->reserved && (entry->age < min_age))
```

## ⏳ Pendente — rebuild completo com kernel patches + userspace fix

### Ordem de build
1. Kernel: compilar com patches 0001-0005 + 0006 no `userpatches/kernel/sun55iw3-current/`
2. Userspace: recompilar com o fix de `RequestBeginPicture` (close + alloc novo request_fd)
3. Reboot

### Observações
- Kernel source: `/home/orangepi/orangepi-build/kernel/orange-pi-5.15-sun55iw3/`
- Também disponível em `refs/orange-pi-5.15-sun55iw3/`
- Requer reboot após instalar módulo para que `sunxi-cedrus` seja carregado com o novo código

---

## ✅ Implementado

### Profile fix
- `v4l2_set_control_simple(video_fd, 0x00990a6b, 4)` em `context.c:143-153`
- Logs confirmam `SET_CTRL_SIMPLE id=0x990a6b value=4`

### SRAM position fix
- `dpb_insert(CurrPic)` movido para **depois** de `h264_translate_and_set_controls`
- DPB enviado ao kernel **não inclui** o frame atual
- Kernel usa `find_first_zero_bit` → posições SRAM únicas

### DPB ts_cache
- `ts_cache` mapeia `VASurfaceID → timestamp`, sobrevive a `dpb_clear_entry()`
- `dpb_update()` usa `ts_cache_get()` ao reinserir referência removida

### ref_pic_list_fields
- `ref_pic_list0_fields[32]`, `ref_pic_list1_fields[32]` em `h264-ctrls.h`
- Preenchidos de VA_PICTURE_H264 flags, usados na tradução

### DPB slot reuse fix (Bug 1)
- `output_idx` salvo antes de `dpb_update()`, ponteiro recuperado depois

### DPB slot theft fix (Bug 4)
- `dpb_find_oldest_unused_entry()` agora pula `reserved` entries
- Slot reservado para CurrPic nunca é roubado por `dpb_update`

### PRED_WEIGHTS unconditional (Bug 2)
- `translate_pred_weights()` sempre chamado
- `V4L2_CID_STATELESS_H264_PRED_WEIGHTS` sempre enviado (zerado se desnecessário)

### Timestamp uniqueness (Bug 3)
- Contador monotônico adicionado ao `tv_usec` em `RequestEndPicture`

### Cleanup
- `src_backup/` deletado, `.gitignore` atualizado
- `capture_requeued`/`v4l2_dequeue_buffer_nonblock` revertidos

### Kernel patch 0005 (cedrus: fix vb2_find_timestamp)
- `refs/patch_cedrus/0005-cedrus-fix-vb2_find_timestamp.patch`
- 6 hunks em `cedrus_h264.c`:
  - `cedrus_write_frame_list()`: adiciona `j`, substitui `vb2_find_timestamp` por loop manual com skip de `run->dst`, remove bloco `if(run->dst->vb2_buf.timestamp == dpb->reference_ts)`, remove `if(output>=0)` conditional
  - `_cedrus_write_ref_list()`: adiciona `j`, substitui `vb2_find_timestamp` por loop manual
- Compilado com sucesso via orangepi-build (kernel 5.15.147 sun55iw3-current)
- Aplica com `patch -p1` contra o `cedrus_h264.c` original do BSP T527

### Bug 5: REINIT no-op → infinite loop fix
- `src/picture.c:225-230`: close old `request_fd` instead of `media_request_reinit()`
- `MEDIA_REQUEST_IOC_REINIT` é no-op no BSP T527
- Sem o fix, request_fd travava no estado COMPLETE → QBUF com request falhava
  → hardware nunca iniciava → POLL timeout → loop infinito

### Kernel patch 0006 (DTS — cedrus compatible clock-names)
- `refs/patch_cedrus/0006-dts-fix-cedrus-compatible.patch`
- `clock-names` = `"ahb", "mod", "ram"` (cedrus espera esses nomes)
- `reg` = range único (remove SRAM mbus, T527 usa NO_SRAM)
- Remove `reset-names` (cedrus usa `devm_reset_control_get(dev, NULL)` sem nome)
