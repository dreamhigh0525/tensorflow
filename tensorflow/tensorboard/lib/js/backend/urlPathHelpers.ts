/* Copyright 2015 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
module TF.Backend.UrlPathHelpers {
  export var BAD_CHARACTERS = "#%&{}\\/<>*? $!'\":@+`|=() ";

  /** Cleanup a url so that it can be loaded from a filesystem. */
  export function clean(s) {
    // for consistency with python's urllib.urlencode
    s = s.replace(new RegExp("%20", "g"), "+");
    for (var i = 0; i < BAD_CHARACTERS.length; i++) {
      var c = BAD_CHARACTERS[i];
      s = s.replace(new RegExp("\\" + c, "g"), "_");
    }
    return s;
  }

  export function queryEncoder(params?: any): string {
    // It's important that the keys be sorted, so we always grab the right file
    // if we are talking to the backend generated by serialze_tensorboard.py
    if (params == null) {
      return "";
    }
    var components = _.keys(params)
                         .sort()
                         .filter((k) => params[k] !== undefined)
                         .map((k) => k + "=" + encodeURIComponent(params[k]));
    var result = components.length ? "?" + components.join("&") : "";
    // Replace parens for consistency with urllib.urlencode
    return result.replace(/\(/g, "%28").replace(/\)/g, "%29");
  }
}
