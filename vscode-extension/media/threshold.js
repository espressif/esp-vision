/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

(() => {
    const vscode = acquireVsCodeApi();
    const sourceImage = document.getElementById("sourceImage");
    const sourceEmpty = document.getElementById("sourceEmpty");
    const maskCanvas = document.getElementById("maskCanvas");
    const tupleText = document.getElementById("tupleText");
    const copyButton = document.getElementById("copyButton");
    const resetButton = document.getElementById("resetButton");
    const freezeButton = document.getElementById("freezeButton");
    const metadata = document.getElementById("metadata");

    const DEFAULTS = { L: [0, 100], A: [-128, 127], B: [-128, 127] };
    const thresholds = { L: [0, 100], A: [-128, 127], B: [-128, 127] };
    let labCache = null;
    let labWidth = 0;
    let labHeight = 0;
    let pendingFrame = null;
    let decodingFrame = false;
    let frozen = false;

    sourceImage.hidden = true;

    function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }

    function srgbToLinear(c) {
        c /= 255;
        return c > 0.04045 ? Math.pow((c + 0.055) / 1.055, 2.4) : c / 12.92;
    }

    function fXyz(t) {
        return t > 0.008856451586 ? Math.cbrt(t) : 7.787037 * t + 16 / 116;
    }

    function computeLab(imageData) {
        const { data, width, height } = imageData;
        const out = new Float32Array(width * height * 3);
        for (let i = 0, j = 0; i < data.length; i += 4, j += 3) {
            const r = srgbToLinear(data[i]);
            const g = srgbToLinear(data[i + 1]);
            const b = srgbToLinear(data[i + 2]);
            const x = (r * 0.4124564 + g * 0.3575761 + b * 0.1804375) / 0.95047;
            const y = (r * 0.2126729 + g * 0.7151522 + b * 0.0721750) / 1.0;
            const z = (r * 0.0193339 + g * 0.1191920 + b * 0.9503041) / 1.08883;
            const fx = fXyz(x);
            const fy = fXyz(y);
            const fz = fXyz(z);
            out[j] = 116 * fy - 16;
            out[j + 1] = 500 * (fx - fy);
            out[j + 2] = 200 * (fy - fz);
        }
        return out;
    }

    function renderMask() {
        if (!labCache || labWidth === 0 || labHeight === 0) {
            return;
        }
        if (maskCanvas.width !== labWidth || maskCanvas.height !== labHeight) {
            maskCanvas.width = labWidth;
            maskCanvas.height = labHeight;
        }
        const ctx = maskCanvas.getContext("2d");
        if (!ctx) {
            return;
        }
        const out = ctx.createImageData(labWidth, labHeight);
        const [Lmin, Lmax] = thresholds.L;
        const [Amin, Amax] = thresholds.A;
        const [Bmin, Bmax] = thresholds.B;
        for (let i = 0, j = 0; i < labCache.length; i += 3, j += 4) {
            const L = labCache[i], A = labCache[i + 1], B = labCache[i + 2];
            const pass = L >= Lmin && L <= Lmax && A >= Amin && A <= Amax && B >= Bmin && B <= Bmax;
            const v = pass ? 255 : 0;
            out.data[j] = v;
            out.data[j + 1] = v;
            out.data[j + 2] = v;
            out.data[j + 3] = 255;
        }
        ctx.putImageData(out, 0, 0);
    }

    function decodeFrame(frame) {
        decodingFrame = true;
        const img = new Image();
        img.onload = () => {
            sourceImage.src = img.src;
            sourceImage.hidden = false;
            sourceEmpty.hidden = true;
            const w = img.naturalWidth, h = img.naturalHeight;
            const tmp = document.createElement("canvas");
            tmp.width = w;
            tmp.height = h;
            const tmpCtx = tmp.getContext("2d", { willReadFrequently: true });
            if (!tmpCtx) {
                decodingFrame = false;
                return;
            }
            tmpCtx.drawImage(img, 0, 0);
            const imageData = tmpCtx.getImageData(0, 0, w, h);
            labCache = computeLab(imageData);
            labWidth = w;
            labHeight = h;
            renderMask();
            metadata.textContent = `${w}x${h} JPG · ${frame.size} bytes · ${frame.fpsText} · ${frame.receivedAtText}`;
            decodingFrame = false;
            if (pendingFrame) {
                const next = pendingFrame;
                pendingFrame = null;
                decodeFrame(next);
            }
        };
        img.onerror = () => {
            decodingFrame = false;
        };
        img.src = "data:image/jpeg;base64," + frame.base64;
    }

    function showFrame(frame) {
        if (frozen) {
            return;
        }
        if (decodingFrame) {
            pendingFrame = frame;
            return;
        }
        decodeFrame(frame);
    }

    function clearFrame() {
        sourceImage.removeAttribute("src");
        sourceImage.hidden = true;
        sourceEmpty.hidden = false;
        const ctx = maskCanvas.getContext("2d");
        if (ctx) {
            ctx.clearRect(0, 0, maskCanvas.width, maskCanvas.height);
        }
        labCache = null;
        labWidth = 0;
        labHeight = 0;
        metadata.textContent = "";
    }

    function updateTupleText() {
        const t = thresholds;
        tupleText.textContent = `(${t.L[0]}, ${t.L[1]}, ${t.A[0]}, ${t.A[1]}, ${t.B[0]}, ${t.B[1]})`;
    }

    function syncRow(row, channel) {
        const sliders = row.querySelectorAll('input[type="range"]');
        const numbers = row.querySelectorAll('input[type="number"]');
        const minSlider = sliders[0], maxSlider = sliders[1];
        const minNumber = numbers[0], maxNumber = numbers[1];
        const lo = Number(minSlider.min), hi = Number(maxSlider.max);

        function commit(loVal, hiVal) {
            loVal = clamp(Math.round(loVal), lo, hi);
            hiVal = clamp(Math.round(hiVal), lo, hi);
            if (loVal > hiVal) {
                [loVal, hiVal] = [hiVal, loVal];
            }
            minSlider.value = String(loVal);
            maxSlider.value = String(hiVal);
            minNumber.value = String(loVal);
            maxNumber.value = String(hiVal);
            thresholds[channel] = [loVal, hiVal];
            updateTupleText();
            renderMask();
        }

        for (const el of [minSlider, minNumber]) {
            el.addEventListener("input", () => commit(Number(el.value), Number(maxSlider.value)));
        }
        for (const el of [maxSlider, maxNumber]) {
            el.addEventListener("input", () => commit(Number(minSlider.value), Number(el.value)));
        }
    }

    function resetThresholds() {
        for (const channel of ["L", "A", "B"]) {
            const row = document.querySelector(`.row[data-channel="${channel}"]`);
            const [lo, hi] = DEFAULTS[channel];
            row.querySelector('input[type="range"][data-bound="min"]').value = String(lo);
            row.querySelector('input[type="range"][data-bound="max"]').value = String(hi);
            row.querySelector('input[type="number"][data-bound="min"]').value = String(lo);
            row.querySelector('input[type="number"][data-bound="max"]').value = String(hi);
            thresholds[channel] = [lo, hi];
        }
        updateTupleText();
        renderMask();
    }

    for (const channel of ["L", "A", "B"]) {
        const row = document.querySelector(`.row[data-channel="${channel}"]`);
        syncRow(row, channel);
    }

    copyButton.addEventListener("click", () => {
        navigator.clipboard.writeText(tupleText.textContent || "").catch(() => undefined);
    });
    resetButton.addEventListener("click", resetThresholds);
    freezeButton.addEventListener("click", () => {
        frozen = !frozen;
        freezeButton.textContent = frozen ? "Resume" : "Freeze";
        freezeButton.classList.toggle("active", frozen);
    });

    window.addEventListener("message", (event) => {
        if (event.data && event.data.type === "frame") {
            showFrame(event.data.frame);
        } else if (event.data && event.data.type === "clear") {
            if (!frozen) {
                clearFrame();
            }
        }
    });

    clearFrame();
    updateTupleText();
    vscode.postMessage({ type: "ready" });
})();
