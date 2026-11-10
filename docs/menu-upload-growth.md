# Menu upload growth

Each frame slot owns separate upload buffers for the Insert menu, FPS overlay and
startup screen. `present_image` waits for that slot's fence before `menu_record`
uses them. The bitmap and its copy are protected by `bmp_mu`.

When a bitmap outgrows its buffer, create the replacement in a local `Upload`.
Only after successful allocation release the old buffer and publish the replacement,
with its copied-bitmap version reset. On allocation failure, leave every field of
the old upload intact and record no copy for the oversized bitmap. A later smaller
bitmap can still use that capacity, and a later growth can retry normally.

Previously the allocator wrote directly into the published handles: successful growth
lost the old allocation, and failed growth could replace its handles with invalid
outputs while leaving old capacity/mapping/version metadata.

The production-function CPU fixture checks allocation growth, exact buffer ownership,
failure preservation, retry, cached capacity/version, pixel bytes and keyed-run offsets.
This does not measure actual driver memory reclamation or replace the frame-slot fence
requirement. No new allocation budget, shrinking policy or synchronization wait is added.
