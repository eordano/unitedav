// SPDX-License-Identifier: Apache-2.0
// UnitedAV Unity WebGL backend: browser-native video decode -> Unity GL texture.
//
// A Unity WebGL build can't load the native FFmpeg plugin, so this backend uses the
// browser's own HTMLVideoElement (hardware-accelerated decode where the browser
// supports it) and uploads each frame into Unity's WebGL texture with
// texImage2D(..., videoElement) — the upload path verified by tests/web/run-webgl.sh
// (WebGL readback == the decoded frame, mean|d|=0). The C# side calls these via
// [DllImport("__Internal")] (see Internal/UnitedAVWebGL.cs).
var UnitedAVWebGL = {
  $UAVW: {
    items: {},
    next: 1,
    get: function (h) { return UnitedAVWebGL.$UAVW.items[h]; },

    // ---- WebGPU NV12/I420 -> RGB conversion (matches the native backends) ----
    // YUV->RGB coefficients, identical to egl_vaapi.cpp coeffs_for()/make_matrix().
    wgpuMat: function (matrix, full, h) {
      var kr, kb;
      if (matrix === 'bt709') { kr = 0.2126; kb = 0.0722; }
      else if (matrix === 'bt470bg' || matrix === 'smpte170m') { kr = 0.299; kb = 0.114; }
      else if (matrix === 'bt2020-ncl' || matrix === 'bt2020-cl') { kr = 0.2627; kb = 0.0593; }
      else { if (h <= 576) { kr = 0.299; kb = 0.114; } else { kr = 0.2126; kb = 0.0722; } }
      var kg = 1 - kr - kb, ys, yb, cs;
      if (full) { ys = 1; yb = 0; cs = 1; }
      else { ys = 255 / (235 - 16); yb = 16 / 255; cs = 255 / (240 - 16); }
      return new Float32Array([
        ys, 0, cs * (2 - 2 * kr), 0,
        ys, -cs * (2 * kb * (1 - kb) / kg), -cs * (2 * kr * (1 - kr) / kg), 0,
        ys, cs * (2 - 2 * kb), 0, 0,
        yb, 0.5, 0.5, 0]);
    },

    // Lazily build + cache the render pipeline + sampler on a given device.
    wgpuPipeline: function (device) {
      if (this._p && this._p.device === device) return this._p;
      var sh = device.createShaderModule({ code:
        'struct VO{@builtin(position) p:vec4f,@location(0) uv:vec2f};' +
        '@vertex fn vs(@builtin(vertex_index) i:u32)->VO{var q=array<vec2f,3>(vec2f(-1,-1),vec2f(3,-1),vec2f(-1,3));' +
        ' var o:VO; o.p=vec4f(q[i],0,1); o.uv=(q[i]*vec2f(0.5,-0.5))+vec2f(0.5); return o;}' +
        'struct CM{m0:vec4f,m1:vec4f,m2:vec4f,yoff:vec4f};' +
        '@group(0)@binding(0) var s:sampler; @group(0)@binding(1) var tY:texture_2d<f32>;' +
        '@group(0)@binding(2) var tU:texture_2d<f32>; @group(0)@binding(3) var tV:texture_2d<f32>;' +
        '@group(0)@binding(4) var<uniform> cm:CM;' +
        '@fragment fn fs(o:VO)->@location(0) vec4f{' +
        ' let y=textureSample(tY,s,o.uv).r; let u=textureSample(tU,s,o.uv).r; let w=textureSample(tV,s,o.uv).r;' +
        ' let ycc=vec3f(y,u,w)-cm.yoff.xyz;' +
        ' let rgb=vec3f(dot(cm.m0.xyz,ycc),dot(cm.m1.xyz,ycc),dot(cm.m2.xyz,ycc));' +
        ' return vec4f(clamp(rgb,vec3f(0),vec3f(1)),1.0);}' });
      var pipeline = device.createRenderPipeline({ layout: 'auto',
        vertex: { module: sh, entryPoint: 'vs' },
        fragment: { module: sh, entryPoint: 'fs', targets: [{ format: 'rgba8unorm' }] },
        primitive: { topology: 'triangle-list' } });
      this._p = { device: device, pipeline: pipeline,
        sampler: device.createSampler({ magFilter: 'linear', minFilter: 'linear' }) };
      return this._p;
    },

    // Convert one decoded VideoFrame (raw planes in u8/layout) into `target`.
    wgpuConvert: function (device, P, target, vf, u8, layout, fmt) {
      var cw = vf.codedWidth, ch = vf.codedHeight, cwh = cw >> 1, chh = ch >> 1;
      function plane(i, pw, ph) { var o = layout[i].offset, st = layout[i].stride, out = new Uint8Array(pw * ph);
        for (var y = 0; y < ph; y++) out.set(u8.subarray(o + y * st, o + y * st + pw), y * pw); return out; }
      var yA, uA, vA;
      if (fmt === 'I420' || fmt === 'I420A') { yA = plane(0, cw, ch); uA = plane(1, cwh, chh); vA = plane(2, cwh, chh); }
      else if (fmt === 'NV12') { yA = plane(0, cw, ch); var o = layout[1].offset, st = layout[1].stride;
        uA = new Uint8Array(cwh * chh); vA = new Uint8Array(cwh * chh);
        for (var y = 0; y < chh; y++) for (var x = 0; x < cwh; x++) { var s = o + y * st + x * 2; uA[y * cwh + x] = u8[s]; vA[y * cwh + x] = u8[s + 1]; } }
      else return;
      function mk(d, pw, ph) { var t = device.createTexture({ size: [pw, ph], format: 'r8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
        device.queue.writeTexture({ texture: t }, d, { bytesPerRow: pw, rowsPerImage: ph }, [pw, ph]); return t; }
      var tY = mk(yA, cw, ch), tU = mk(uA, cwh, chh), tV = mk(vA, cwh, chh);
      var cs = vf.colorSpace || {};
      var cbuf = device.createBuffer({ size: 64, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
      device.queue.writeBuffer(cbuf, 0, this.wgpuMat(cs.matrix, !!cs.fullRange, ch));
      var bg = device.createBindGroup({ layout: P.pipeline.getBindGroupLayout(0), entries: [
        { binding: 0, resource: P.sampler }, { binding: 1, resource: tY.createView() },
        { binding: 2, resource: tU.createView() }, { binding: 3, resource: tV.createView() },
        { binding: 4, resource: { buffer: cbuf } }] });
      var enc = device.createCommandEncoder();
      var pass = enc.beginRenderPass({ colorAttachments: [{ view: target.createView(), loadOp: 'clear', storeOp: 'store', clearValue: { r: 0, g: 0, b: 0, a: 1 } }] });
      pass.setPipeline(P.pipeline); pass.setBindGroup(0, bg); pass.draw(3); pass.end();
      device.queue.submit([enc.finish()]);
    },
  },

  UAV_Web_Create: function (urlPtr) {
    var url = UTF8ToString(urlPtr);
    var v = document.createElement('video');
    v.crossOrigin = 'anonymous';
    v.playsInline = true;
    v.preload = 'auto';
    v.muted = true;            // start muted so autoplay is allowed; SetVolume>0 unmutes
    var rec = { v: v, hasFrame: false, ended: false, error: 0 };
    v.addEventListener('error', function () { rec.error = 1; });
    v.addEventListener('ended', function () { rec.ended = true; });
    if (v.requestVideoFrameCallback) {
      var cb = function () { rec.hasFrame = true; v.requestVideoFrameCallback(cb); };
      v.requestVideoFrameCallback(cb);
    } else {
      rec.hasFrame = true;     // no rVFC: assume a frame is current once readyState>=2
    }
    v.src = url;
    var h = UnitedAVWebGL.$UAVW.next++;
    UnitedAVWebGL.$UAVW.items[h] = rec;
    return h;
  },

  UAV_Web_Play: function (h) { var r = UnitedAVWebGL.$UAVW.get(h); if (r) r.v.play().catch(function () {}); },
  UAV_Web_Pause: function (h) { var r = UnitedAVWebGL.$UAVW.get(h); if (r) r.v.pause(); },
  UAV_Web_Seek: function (h, t) { var r = UnitedAVWebGL.$UAVW.get(h); if (r) r.v.currentTime = t; },
  UAV_Web_SetLooping: function (h, on) { var r = UnitedAVWebGL.$UAVW.get(h); if (r) r.v.loop = !!on; },
  UAV_Web_SetVolume: function (h, vol) { var r = UnitedAVWebGL.$UAVW.get(h); if (r) { r.v.volume = Math.max(0, Math.min(1, vol)); if (vol > 0) r.v.muted = false; } },
  UAV_Web_SetMuted: function (h, on) { var r = UnitedAVWebGL.$UAVW.get(h); if (r) r.v.muted = !!on; },

  UAV_Web_GetWidth: function (h) { var r = UnitedAVWebGL.$UAVW.get(h); return r ? (r.v.videoWidth | 0) : 0; },
  UAV_Web_GetHeight: function (h) { var r = UnitedAVWebGL.$UAVW.get(h); return r ? (r.v.videoHeight | 0) : 0; },
  UAV_Web_GetDuration: function (h) { var r = UnitedAVWebGL.$UAVW.get(h); if (!r) return 0; return isFinite(r.v.duration) ? r.v.duration : -1.0; },
  UAV_Web_GetPosition: function (h) { var r = UnitedAVWebGL.$UAVW.get(h); return r ? r.v.currentTime : 0.0; },
  UAV_Web_HasVideo: function (h) { var r = UnitedAVWebGL.$UAVW.get(h); return (r && r.v.videoWidth > 0) ? 1 : 0; },

  // Matches the C ABI's UAVState: 0 idle,1 opening,2 ready,3 playing,4 paused,
  // 5 buffering/seeking,6 finished,7 error.
  UAV_Web_GetState: function (h) {
    var r = UnitedAVWebGL.$UAVW.get(h);
    if (!r) return 7;
    var v = r.v;
    if (r.error || v.networkState === 3 /* NETWORK_NO_SOURCE */) return 7;
    if (r.ended || v.ended) return 6;
    if (v.seeking) return 5;
    if (v.readyState < 2 /* HAVE_CURRENT_DATA */) return 1;
    if (!v.paused) return 3;
    return v.currentTime > 0 ? 4 : 2;
  },

  // True if a new decoded frame is available since the last upload.
  UAV_Web_HasNewFrame: function (h) { var r = UnitedAVWebGL.$UAVW.get(h); return (r && r.hasFrame && r.v.readyState >= 2) ? 1 : 0; },

  // Upload the current frame into Unity's GL texture. glTex is
  // (int)Texture.GetNativeTexturePtr() — an index into Emscripten's GL.textures.
  // Returns 1 if a frame was uploaded. UNPACK_FLIP_Y so Unity's bottom-up UVs show
  // the image upright (so ITextureProducer.RequiresVerticalFlip() is false here).
  UAV_Web_UploadTexture: function (h, glTex) {
    var r = UnitedAVWebGL.$UAVW.get(h);
    if (!r) return 0;
    var v = r.v;
    if (v.readyState < 2 || v.videoWidth === 0) return 0;
    var tex = GL.textures[glTex];
    if (!tex) return 0;
    try {
      GLctx.bindTexture(GLctx.TEXTURE_2D, tex);
      GLctx.pixelStorei(GLctx.UNPACK_FLIP_Y_WEBGL, true);
      GLctx.texImage2D(GLctx.TEXTURE_2D, 0, GLctx.RGBA, GLctx.RGBA, GLctx.UNSIGNED_BYTE, v);
      GLctx.pixelStorei(GLctx.UNPACK_FLIP_Y_WEBGL, false);
      r.hasFrame = false;
      return 1;
    } catch (e) { return 0; }
  },

  // WebGPU upload — CORRECT-color path (compiled into a Unity *WebGPU* player build).
  // Chromium's copyExternalImageToTexture / importExternalTexture apply their own
  // YUV->RGB that ignores the clip's tagged matrix (a BT.601-vs-709 drift; see
  // tests/web/README.md), so they DON'T match the browser's own <video> presentation.
  // We therefore do the conversion ourselves, exactly like the native GPU backends:
  // WebCodecs VideoFrame.copyTo -> raw Y/U/V planes -> a WGSL shader applies the
  // clip's signalled matrix+range. Verified standalone in tests/web/webgpu_probe.html
  // (mode=webcodecs): vs the browser presentation mean|d|=0.387, vs ffmpeg ~2.7-3.1
  // on apple/metal-3 AND intel/gen-12lp. Cost: a per-frame plane copyTo — WebGPU
  // exposes no zero-copy access to a video frame's raw YUV planes (the only zero-copy
  // import, importExternalTexture, hides the YUV behind the browser's own conversion).
  //
  // RUNTIME-VERIFY TODO (needs a Unity WebGPU player build to confirm): how Unity
  // exposes its GPUDevice + the target GPUTexture to emscripten (registry names are
  // best-effort below) and that the target texture carries RENDER_ATTACHMENT usage.
  // The conversion math/shader is the verified one; only this Unity glue is unproven.
  UAV_Web_UploadTextureWGPU: function (h, wgpuTex) {
    var r = UnitedAVWebGL.$UAVW.get(h);
    if (!r) return 0;
    var v = r.v;
    if (v.readyState < 2 || v.videoWidth === 0 || typeof VideoFrame === 'undefined') return 0;
    var device = Module['preinitializedWebGPUDevice'] ||
                 (typeof WebGPU !== 'undefined' ? WebGPU.preinitializedDevice : null);
    var texTable = (typeof WebGPU !== 'undefined' && WebGPU.mgrTexture) ? WebGPU.mgrTexture
                 : (Module['webgpuTextures'] || null);
    if (!device || !texTable) return 0;
    var target = texTable.get ? texTable.get(wgpuTex) : texTable[wgpuTex];
    if (!target) return 0;

    var P = UnitedAVWebGL.$UAVW.wgpuPipeline(device);  // lazily built + cached
    if (!P) return 0;
    try {
      var vf = new VideoFrame(v);
      // The colour-correct path needs raw Y/U/V planes. HW-decoded frames hide them
      // (format=null) and copyTo can't convert TO i420, so skip those (the player
      // build should run with software decode, or use the WebGL/browser-conversion
      // fallback). Known planar formats only.
      var fmt = vf.format;
      if (fmt !== 'I420' && fmt !== 'I420A' && fmt !== 'NV12') { vf.close(); return 0; }
      var ab = new ArrayBuffer(vf.allocationSize());
      // copyTo is async; this synchronous ABI presents the frame on the next tick.
      // A production Unity build should drive this from requestVideoFrameCallback.
      vf.copyTo(ab).then(function (layout) {
        try { UnitedAVWebGL.$UAVW.wgpuConvert(device, P, target, vf, new Uint8Array(ab), layout, fmt); }
        finally { vf.close(); r.hasFrame = false; }
      }).catch(function () { try { vf.close(); } catch (e) {} });
      return 1;
    } catch (e) { return 0; }
  },

  UAV_Web_Destroy: function (h) {
    var items = UnitedAVWebGL.$UAVW.items, r = items[h];
    if (r) { try { r.v.pause(); r.v.removeAttribute('src'); r.v.load(); } catch (e) {} delete items[h]; }
  },
};

autoAddDeps(UnitedAVWebGL, '$UAVW');
mergeInto(LibraryManager.library, UnitedAVWebGL);
