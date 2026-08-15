// HeliosView bridge shim — injected into every document via
// AddScriptToExecuteOnDocumentCreated (embedded into HeliosView.dll from this
// file with C++23 #embed, see kWebView2BridgeScript in heliosview_win32.cpp).
//
// Wire format (shared with the C side — see the comment near hv_valid_name in
// heliosview_win32.cpp): a "HV" magic + tab-separated kind and fields, then a
// "\r\n\r\n" separator (HTTP-style) and a payload passed through verbatim.
// Registered names are C identifiers, so the header fields never contain a tab
// or CR/LF; the payload (JSON text for args/result/data, or a native error
// object) may contain anything.
//
// This file is plain ES5 (no modules, no transpilation): it runs in the page
// before any app script, so it must not rely on newer syntax.
(function () {
  'use strict';
  if (window.__hvShim) return;        /* installed already (subframe/navigation) */
  window.__hvShim = 1;

  var pending = new Map();            /* id -> {resolve, reject} */
  var seq = 0;

  function pack(kind, fields, payload) {
    var s = 'HV\t' + kind;
    for (var i = 0; i < fields.length; i++) s += '\t' + fields[i];
    return s + '\r\n\r\n' + (payload === undefined ? '' : payload);
  }
  function post(s) { window.chrome.webview.postMessage(s); }

  /* Native -> page: parse an envelope string and dispatch. */
  function recv(e) {
    var m = e.data;
    if (typeof m !== 'string') return;
    var sep = m.indexOf('\r\n\r\n');
    if (sep < 0) return;
    var head = m.slice(0, sep);       /* "HV\t<kind>\t<...fields>" */
    var body = m.slice(sep + 4);
    var h = head.split('\t');
    if (h.length < 2 || h[0] !== 'HV') return;
    if (h[1] === 'resolve' || h[1] === 'reject') {
      var id = Number(h[2]);
      var p = pending.get(id);
      if (!p) return;
      pending.delete(id);
      if (h[1] === 'resolve') {
        try { p.resolve(body === '' ? null : JSON.parse(body)); }
        catch (e2) { p.reject(e2); }
      } else {
        var err;
        try {
          var obj = JSON.parse(body);
          /* native rejects use {"error": ...} (errorToJson / JsonError); fall
             back to "message" for other payload shapes */
          err = new Error((obj && (obj.message || obj.error)) || 'bridge error');
        }
        catch (e3) { err = new Error(body || 'bridge error'); }
        p.reject(err);
      }
    } else if (h[1] === 'broadcast' && h.length >= 3) {
      var data = null;
      try { data = body === '' ? null : JSON.parse(body); } catch (e4) {}
      dispatchBC(h[2], data);
    }
  }

  window.chrome.webview.addEventListener('message', recv);

  window.helios = {
    call: function (name) {
      var args = Array.prototype.slice.call(arguments, 1);
      return new Promise(function (resolve, reject) {
        var id = ++seq;
        pending.set(id, { resolve: resolve, reject: reject });
        post(pack('call', [String(id), name], JSON.stringify(args)));
      });
    }
  };

  /* BroadcastChannel: keep the native broadcast and the standard same-origin
     channel working together by subclassing. A native broadcast dispatches a
     synthetic MessageEvent on matching instances; a page postMessage is forwarded
     to native (subscribe) and still delivered to the other same-origin tabs. */
  var NativeBC = window.BroadcastChannel;
  var live = new Set();
  function dispatchBC(name, data) {
    live.forEach(function (ch) {
      if (ch._hvName === name)
        ch.dispatchEvent(new MessageEvent('message', { data: data }));
    });
  }
  window.BroadcastChannel = function (name) {
    var ch = new NativeBC(name);
    ch._hvName = name;
    live.add(ch);
    var origPost = ch.postMessage.bind(ch);
    var origClose = ch.close.bind(ch);
    ch.postMessage = function (data) {
      post(pack('broadcast', [name], JSON.stringify(data === undefined ? null : data)));
      return origPost(data);
    };
    ch.close = function () { live.delete(ch); return origClose(); };
    return ch;
  };
  window.BroadcastChannel.prototype = NativeBC.prototype;

  /* Built-in window-control buttons as a web component (<helios-window-controls>).
     Pages that draw their own title bar place the element (usually inside it);
     it floats at the top-right (position:fixed, 48px tall, 46px per button) and
     draws the Win10/11 min/max/close glyphs with hover/pressed feedback. Actions
     go through the built-in __hv.control / __hv.state bridge methods (registered
     natively; the "__hv." names are not valid C identifiers, so they cannot be
     bound or subscribed by applications). Restyle via CSS on the
     element (height/top/right/color/background); the glyph color follows
     currentColor. */
  if (typeof customElements !== 'undefined' && !customElements.get('helios-window-controls')) {
    var WC = function () { return Reflect.construct(HTMLElement, [], WC); };
    WC.prototype = Object.create(HTMLElement.prototype);
    WC.prototype.constructor = WC;
    WC.prototype.connectedCallback = function () {
      if (this._hvDone) return;
      this._hvDone = true;
      var host = this;
      var G = { min: '\uE921', max: '\uE922', restore: '\uE923', close: '\uE8BB' };
      var st = host.style;
      st.cssText = 'position:fixed;top:0;right:0;height:48px;display:flex;' +
        'align-items:center;user-select:none;-webkit-user-select:none;' +
        'z-index:2147483647;font-family:"Segoe MDL2 Assets";font-size:10px;color:inherit;' +
        'app-region:no-drag;-webkit-app-region:no-drag;';
      function refreshMax() {
        window.helios.call('__hv.state').then(function (s) {
          var b = host._maxBtn;
          if (!b || !s) return;
          b.textContent = s.maximized ? G.restore : G.max;
          /* maximize is disabled while the window cannot be maximized (e.g.
             resizing was locked via setResizable); restore always stays
             available so a maximized window can be brought back */
          var can = s.maximized || s.maximizable !== false;
          if (can) {
            b.style.opacity = '';
            b.style.pointerEvents = '';
            b.style.cursor = '';
          } else {
            b.style.opacity = '.35';
            b.style.pointerEvents = 'none';
            b.style.cursor = 'default';
          }
        }).catch(function () {});
      }
      function mk(action, glyph) {
        var b = document.createElement('div');
        b.textContent = glyph;
        b.style.cssText = 'width:46px;height:100%;display:flex;align-items:center;' +
          'justify-content:center;';
        b.onmouseenter = function () {
          b.style.background = action === 'close' ? '#e81123' : 'rgba(128,128,128,.25)';
          if (action === 'close') b.style.color = '#fff';
          if (action === 'maximize') refreshMax();
        };
        b.onmouseleave = function () {
          b.style.background = 'transparent';
          b.style.color = '';
        };
        b.onmousedown = function (e) { e.preventDefault(); e.stopPropagation(); };
        b.onclick = function (e) {
          e.stopPropagation();
          window.helios.call('__hv.control', action).catch(function () {});
          if (action === 'maximize') refreshMax();
        };
        host.appendChild(b);
        return b;
      }
      mk('minimize', G.min);
      host._maxBtn = mk('maximize', G.max);
      mk('close', G.close);
      refreshMax();
      /* Keep the button in sync when the window state changes behind the page
         (setResizable / maximize from other paths); a 500ms poll is a cheap
         local bridge round-trip. */
      host._hvTimer = setInterval(refreshMax, 500);
    };
    WC.prototype.disconnectedCallback = function () {
      if (this._hvTimer) {
        clearInterval(this._hvTimer);
        this._hvTimer = null;
      }
    };
    customElements.define('helios-window-controls', WC);
  }

  /* <helios-window-title-bar>: an in-flow title-bar strip that drags the window
     through WebView2's native app-region support (the library enables
     IsNonClientRegionSupportEnabled — no bridge call involved) and double-clicks
     to toggle maximize (best-effort: drag regions may swallow page events).
     Place it at the top of the page and put <helios-window-controls> inside it
     for the buttons (they opt out with app-region:no-drag automatically). Other
     interactive children need app-region:no-drag to stay clickable. Default
     height 48px / flex row; app inline styles win. */
  if (typeof customElements !== 'undefined' && !customElements.get('helios-window-title-bar')) {
    var TB = function () { return Reflect.construct(HTMLElement, [], TB); };
    TB.prototype = Object.create(HTMLElement.prototype);
    TB.prototype.constructor = TB;
    TB.prototype.connectedCallback = function () {
      if (this._hvDone) return;
      this._hvDone = true;
      var host = this;
      host.style.cssText = 'display:flex;align-items:center;height:48px;flex:0 0 48px;' +
        'user-select:none;-webkit-user-select:none;cursor:default;' +
        'app-region:drag;-webkit-app-region:drag;' +
        (host.style.cssText ? ' ' + host.style.cssText : '');
      host.addEventListener('dblclick', function (e) {
        var t = e.target;
        if (t && t !== host && t.closest &&
            t.closest('button, a, input, select, textarea, helios-window-controls, [data-hv-no-drag]'))
          return;
        window.helios.call('__hv.control', 'maximize').catch(function () {});
      });
    };
    customElements.define('helios-window-title-bar', TB);
  }
})();
