// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
// SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

// Fluidd keeps its Moonraker access token in localStorage (user-token-<hash>).
// Mirror it into a cookie so <img>/MJPEG/WebRTC camera requests and the
// protected pages (/firmware-config/, ...) carry it and nginx can check it
// with auth_request.
//
// nginx sends visitors who are not logged in to / and remembers the page in
// the login_next cookie. Once the token is valid, go back to that page.
(function () {
  var COOKIE = 'moonraker_token';
  var NEXT_COOKIE = 'login_next';
  var lastToken = null;
  var lastWrite = 0;
  var checking = false;

  function secureAttr() {
    return location.protocol === 'https:' ? '; Secure' : '';
  }

  function findToken() {
    try {
      for (var i = 0; i < localStorage.length; i++) {
        var key = localStorage.key(i);
        if (key && key.indexOf('user-token-') === 0) {
          var value = localStorage.getItem(key);
          if (value) return value;
        }
      }
    } catch (e) { /* storage blocked */ }
    return '';
  }

  function readCookie(name) {
    var parts = document.cookie.split('; ');
    for (var i = 0; i < parts.length; i++) {
      if (parts[i].indexOf(name + '=') === 0) return parts[i].substring(name.length + 1);
    }
    return '';
  }

  function writeCookie(token) {
    var attrs = '; Path=/; SameSite=Strict' + secureAttr();
    if (token) {
      document.cookie = COOKIE + '=' + encodeURIComponent(token) + attrs;
    } else {
      document.cookie = COOKIE + '=; Max-Age=0' + attrs;
    }
    lastWrite = Date.now();
  }

  // Same-origin absolute paths only; never "//host" or "/\host"
  function safeNext() {
    var raw = readCookie(NEXT_COOKIE);
    if (!raw) return '';
    var next;
    try { next = decodeURIComponent(raw); } catch (e) { return ''; }
    if (!/^\/(?![\/\\])[^\s\x00-\x1f]*$/.test(next)) return '';
    if (next === '/' || next === location.pathname + location.search) return '';
    return next;
  }

  function returnToPage(token) {
    var next = safeNext();
    if (!next || checking) return;
    checking = true;
    // Only leave once Moonraker accepts the token, so a stale one cannot loop
    fetch('/access/user', { headers: { Authorization: 'Bearer ' + token } })
      .then(function (r) {
        if (!r.ok) return;
        document.cookie = NEXT_COOKIE + '=; Max-Age=0; Path=/; SameSite=Lax' + secureAttr();
        location.replace(next);
      })
      .catch(function () { /* stay on Fluidd */ })
      .then(function () { checking = false; });
  }

  function tick() {
    var token = findToken();
    var changed = token !== lastToken;
    if (changed || Date.now() - lastWrite > 30000) writeCookie(token);
    if (changed && token) returnToPage(token);
    lastToken = token;
  }

  tick();
  setInterval(tick, 1000);
  window.addEventListener('storage', tick);
})();
