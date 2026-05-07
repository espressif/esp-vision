/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

(() => {
    const vscode = acquireVsCodeApi();
    const empty = document.getElementById("empty");
    const content = document.getElementById("content");
    const image = document.getElementById("previewImage");
    const metadata = document.getElementById("metadata");
    const redCanvas = document.getElementById("histogramRed");
    const greenCanvas = document.getElementById("histogramGreen");
    const blueCanvas = document.getElementById("histogramBlue");
    const redStats = document.getElementById("redStats");
    const greenStats = document.getElementById("greenStats");
    const blueStats = document.getElementById("blueStats");
    const DECODE_TIMEOUT_MS = 2000;
    let currentFrame = null;
    let pendingFrame = null;
    let decodingFrame = false;
    let decodeTimer = null;
    let sourceCanvas = null;
    let sourceCtx = null;

    function resizeCanvas(target) {
        const rect = target.getBoundingClientRect();
        const dpr = Math.max(1, window.devicePixelRatio || 1);
        const width = Math.max(256, Math.floor(rect.width * dpr));
        const height = Math.max(64, Math.floor(rect.height * dpr));
        if (target.width !== width || target.height !== height) {
            target.width = width;
            target.height = height;
        }
        return { width, height, dpr };
    }

    function drawChannel(ctx, values, maxValue, color, size) {
        const left = 30 * size.dpr;
        const right = 8 * size.dpr;
        const top = 14 * size.dpr;
        const bottom = 22 * size.dpr;
        const width = size.width - left - right;
        const height = size.height - top - bottom;
        ctx.beginPath();
        for (let i = 0; i < values.length; i++) {
            const x = left + ((i / (values.length - 1)) * width);
            const y = top + height - ((values[i] / maxValue) * height);
            if (i === 0) {
                ctx.moveTo(x, y);
            } else {
                ctx.lineTo(x, y);
            }
        }
        ctx.strokeStyle = color;
        ctx.lineWidth = Math.max(1, size.dpr);
        ctx.stroke();
    }

    function drawAxes(ctx, maxValue, size) {
        const left = 30 * size.dpr;
        const right = 8 * size.dpr;
        const top = 14 * size.dpr;
        const bottom = 22 * size.dpr;
        const width = size.width - left - right;
        const height = size.height - top - bottom;
        const styles = getComputedStyle(document.documentElement);
        const foreground = styles.getPropertyValue("--vscode-descriptionForeground") || "rgba(160, 160, 160, 0.85)";
        const grid = styles.getPropertyValue("--vscode-panel-border") || "rgba(128, 128, 128, 0.35)";
        ctx.font = (10 * size.dpr) + "px sans-serif";
        ctx.textBaseline = "middle";
        ctx.fillStyle = foreground;
        ctx.strokeStyle = grid;
        ctx.lineWidth = Math.max(1, size.dpr);
        for (const tick of [0, 64, 128, 192, 255]) {
            const x = left + ((tick / 255) * width);
            ctx.beginPath();
            ctx.moveTo(x, top);
            ctx.lineTo(x, top + height);
            ctx.stroke();
            ctx.textAlign = tick === 0 ? "left" : tick === 255 ? "right" : "center";
            ctx.fillText(String(tick), x, top + height + (12 * size.dpr));
        }
        ctx.textAlign = "right";
        ctx.fillText(String(maxValue), left - (5 * size.dpr), top);
        ctx.fillText("0", left - (5 * size.dpr), top + height);
    }

    function drawHistogram(target, values, color) {
        const canvas = target;
        const ctx = canvas.getContext("2d");
        if (!ctx) {
            return;
        }
        const size = resizeCanvas(canvas);
        const styles = getComputedStyle(document.documentElement);
        const border = styles.getPropertyValue("--vscode-panel-border") || "rgba(128, 128, 128, 0.35)";
        const maxValue = Math.max(1, ...values);
        ctx.clearRect(0, 0, size.width, size.height);
        ctx.fillStyle = styles.getPropertyValue("--vscode-sideBar-background") || "transparent";
        ctx.fillRect(0, 0, size.width, size.height);
        ctx.strokeStyle = border;
        ctx.lineWidth = Math.max(1, size.dpr);
        ctx.strokeRect(0.5 * size.dpr, 0.5 * size.dpr, size.width - size.dpr, size.height - size.dpr);
        drawAxes(ctx, maxValue, size);
        drawChannel(ctx, values, maxValue, color, size);
    }

    function updateStats(target, values, totalPixels) {
        if (!target || totalPixels <= 0) {
            return;
        }
        let min = 0;
        let max = 0;
        let weighted = 0;
        for (let i = 0; i < values.length; i++) {
            const value = values[i];
            if (value > 0) {
                max = i;
                if (min === 0 && i > 0) {
                    min = i;
                }
            }
            weighted += value * i;
        }
        const mean = weighted / totalPixels;
        target.textContent = "min " + min + " · max " + max + " · mean " + mean.toFixed(1);
    }

    function computeHistogram() {
        if (!image || !redCanvas || !greenCanvas || !blueCanvas || !redStats || !greenStats || !blueStats || image.naturalWidth <= 0 || image.naturalHeight <= 0) {
            return;
        }
        try {
            const w = image.naturalWidth, h = image.naturalHeight;
            if (!sourceCanvas || sourceCanvas.width !== w || sourceCanvas.height !== h) {
                sourceCanvas = document.createElement("canvas");
                sourceCanvas.width = w;
                sourceCanvas.height = h;
                sourceCtx = sourceCanvas.getContext("2d", { willReadFrequently: true });
            }
            if (!sourceCtx) {
                return;
            }
            sourceCtx.drawImage(image, 0, 0);
            const pixels = sourceCtx.getImageData(0, 0, w, h).data;
            const red = new Array(256).fill(0);
            const green = new Array(256).fill(0);
            const blue = new Array(256).fill(0);
            for (let i = 0; i < pixels.length; i += 4) {
                red[pixels[i]] += 1;
                green[pixels[i + 1]] += 1;
                blue[pixels[i + 2]] += 1;
            }
            const totalPixels = w * h;
            drawHistogram(redCanvas, red, "rgba(255, 82, 82, 0.95)");
            drawHistogram(greenCanvas, green, "rgba(77, 208, 120, 0.95)");
            drawHistogram(blueCanvas, blue, "rgba(88, 166, 255, 0.95)");
            updateStats(redStats, red, totalPixels);
            updateStats(greenStats, green, totalPixels);
            updateStats(blueStats, blue, totalPixels);
        } catch {
        }
    }

    function finishDecode() {
        if (decodeTimer !== null) {
            clearTimeout(decodeTimer);
            decodeTimer = null;
        }
        decodingFrame = false;
        if (pendingFrame) {
            const next = pendingFrame;
            pendingFrame = null;
            showFrame(next);
        }
    }

    function renderFrame(frame) {
        empty.hidden = true;
        content.hidden = false;
        metadata.textContent = frame.width + "x" + frame.height + " JPG · " + frame.size + " bytes · " + frame.fpsText + " · " + frame.receivedAtText;
        image.src = "data:image/jpeg;base64," + frame.base64;
        decodeTimer = setTimeout(finishDecode, DECODE_TIMEOUT_MS);
        const decode = typeof image.decode === "function" ? image.decode() : Promise.resolve();
        decode.then(() => {
            computeHistogram();
        }).catch(() => undefined).finally(finishDecode);
    }

    function showFrame(frame) {
        currentFrame = frame;
        if (decodingFrame) {
            pendingFrame = frame;
            return;
        }
        decodingFrame = true;
        renderFrame(frame);
    }

    function clearFrame() {
        currentFrame = null;
        image.removeAttribute("src");
        metadata.textContent = "";
        redStats.textContent = "";
        greenStats.textContent = "";
        blueStats.textContent = "";
        empty.hidden = false;
        content.hidden = true;
    }

    window.addEventListener("resize", () => {
        if (currentFrame) {
            computeHistogram();
        }
    });
    window.addEventListener("message", (event) => {
        if (event.data && event.data.type === "frame") {
            showFrame(event.data.frame);
        } else if (event.data && event.data.type === "clear") {
            clearFrame();
        }
    });

    clearFrame();
    vscode.postMessage({ type: "ready" });
})();
