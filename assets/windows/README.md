# Approved Windows icon

`reliquary-source.png` was explicitly approved by the user on 2026-09-14 for public packaging, including its existing gradients and lighting. Do not redesign it or replace it with a Qt/legacy icon.

`reliquary.ico` preserves the supplied image, centered on a transparent square without cropping. It contains 16, 24, 32, 48, 64, 128 and 256 pixel square PNG frames. Source SHA-256: `3F2BA698E15A75D4F1A0BC6089A51A6A912AC263A4C11485DC954A2170FAF40C`.

Reproduction with Pillow: open as RGBA, pad to max(width,height), center, and save as ICO with the seven sizes above. The packaging PE validator compares all embedded icon payloads byte for byte with this ICO.
