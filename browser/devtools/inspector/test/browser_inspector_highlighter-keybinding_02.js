/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

// Test that the keybindings for Picker work alright

const TEST_URL = TEST_URL_ROOT + "doc_inspector_highlighter_dom.html";

add_task(function*() {
  let {inspector, toolbox} = yield openInspectorForURL(TEST_URL);

  info("Starting element picker");
  yield toolbox.highlighterUtils.startPicker();

  // Previously chosen child memory
  info("Testing whether previously chosen child is remembered");

  info("Selecting the ahoy paragraph DIV");
  yield moveMouseOver("#ahoy");

  let highlightedNode = yield getHighlitNode(toolbox);

  yield doKeyHover({key: "VK_LEFT", options: {}});
  highlightedNode = yield getHighlitNode(toolbox);
  is(highlightedNode.id, "simple-div2", "The highlighter shows #simple-div2. OK.");

  yield doKeyHover({key: "VK_RIGHT", options: {}});
  highlightedNode = yield getHighlitNode(toolbox);
  is(highlightedNode.id, "ahoy", "The highlighter shows #ahoy. OK.");

  info("Going back up to the complex-div DIV");
  yield doKeyHover({key: "VK_LEFT", options: {}});
  yield doKeyHover({key: "VK_LEFT", options: {}});
  highlightedNode = yield getHighlitNode(toolbox);
  is(highlightedNode.id, "complex-div", "The highlighter shows #complex-div. OK.");

  yield doKeyHover({key: "VK_RIGHT", options: {}});
  highlightedNode = yield getHighlitNode(toolbox);
  is(highlightedNode.id, "simple-div2", "The highlighter shows #simple-div2. OK.");

  info("Previously chosen child is remembered. Passed.");

  info("Stopping the picker");
  yield toolbox.highlighterUtils.stopPicker();

  function doKeyHover(msg) {
    info("Key pressed. Waiting for element to be highlighted/hovered");
    executeInContent("Test:SynthesizeKey", msg);
    return inspector.toolbox.once("picker-node-hovered");
  }

  function moveMouseOver(selector) {
    info("Waiting for element " + selector + " to be highlighted");
    executeInContent("Test:SynthesizeMouse", {
      options: {type: "mousemove"},
      center: true,
      selector: selector
    }, null, false);
    return inspector.toolbox.once("picker-node-hovered");
  }

});
