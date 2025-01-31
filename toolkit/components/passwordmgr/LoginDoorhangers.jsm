/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

this.EXPORTED_SYMBOLS = [
  "LoginDoorhangers",
];

const { classes: Cc, interfaces: Ci, utils: Cu, results: Cr } = Components;

Cu.import("resource://gre/modules/Services.jsm");

const XUL_NS = "http://www.mozilla.org/keymaster/gatekeeper/there.is.only.xul";

this.LoginDoorhangers = {};

/**
 * Doorhanger for selecting and filling logins.
 *
 * @param {Object} properties
 *        Properties from this object will be applied to the new instance.
 */
this.LoginDoorhangers.FillDoorhanger = function (properties) {
  this.onFilterInput = this.onFilterInput.bind(this);
  this.onListDblClick = this.onListDblClick.bind(this);
  this.onListKeyPress = this.onListKeyPress.bind(this);

  this.filterString = properties.filterString;

  if (properties.browser) {
    this.browser = properties.browser;
  }
};

this.LoginDoorhangers.FillDoorhanger.prototype = {
  /**
   * Whether the elements for this doorhanger are currently in the document.
   */
  bound: false,

  /**
   * Associates the doorhanger with its browser. When the tab associated to this
   * browser is selected, the anchor icon for the doorhanger will appear.
   *
   * The browser may change during the lifetime of the doorhanger, in case the
   * web page is moved to a different chrome window by the swapDocShells method.
   */
  set browser(browser) {
    this._browser = browser;

    let doorhanger = this;
    let PopupNotifications = this.chomeDocument.defaultView.PopupNotifications;
    let notification = PopupNotifications.show(
      browser,
      "password-fill",
      "",
      "password-notification-icon",
      null,
      null,
      {
        dismissed: true,
        persistWhileVisible: true,
        eventCallback: function (topic, otherBrowser) {
          switch (topic) {
            case "shown":
              // Since we specified the "dismissed" option, this event will only
              // be called after the "show" method returns, so the reference to
              // "this.notification" will be available at this point.
              doorhanger.bound = true;
              doorhanger.bind();
              break;

            case "dismissed":
            case "removed":
              if (doorhanger.bound) {
                doorhanger.unbind();
              }
              break;

            case "swapping":
              this._browser = otherBrowser;
              return true;
          }
          return false;
        },
      }
    );

    this.notification = notification;
    notification.doorhanger = this;
  },
  get browser() {
    return this._browser;
  },
  _browser: null,

  /**
   * DOM document to which the doorhanger is currently associated.
   *
   * This may change during the lifetime of the doorhanger, in case the web page
   * is moved to a different chrome window by the swapDocShells method.
   */
  get chomeDocument() {
    return this.browser.ownerDocument;
  },

  /**
   * Hides this notification, if the notification panel is currently open.
   */
  hide() {
    let PopupNotifications = this.chomeDocument.defaultView.PopupNotifications;
    if (PopupNotifications.isPanelOpen) {
      PopupNotifications.panel.hidePopup();
    }
  },

  /**
   * Removes the doorhanger from the browser.
   */
  remove() {
    this.notification.remove();
  },

  /**
   * Binds this doorhanger to its UI controls.
   */
  bind() {
    this.element = this.chomeDocument.getElementById("login-fill-doorhanger");
    this.list = this.chomeDocument.getElementById("login-fill-list");
    this.filter = this.chomeDocument.getElementById("login-fill-filter");

    this.filter.setAttribute("value", this.filterString);

    this.refreshList();

    this.filter.addEventListener("input", this.onFilterInput);
    this.list.addEventListener("dblclick", this.onListDblClick);
    this.list.addEventListener("keypress", this.onListKeyPress);

    // Move the main element to the notification panel for displaying.
    this.notification.owner.panel.firstElementChild.appendChild(this.element);
    this.element.hidden = false;
  },

  /**
   * Unbinds this doorhanger from its UI controls.
   */
  unbind() {
    this.filter.removeEventListener("input", this.onFilterInput);
    this.list.removeEventListener("dblclick", this.onListDblClick);
    this.list.removeEventListener("keypress", this.onListKeyPress);

    this.clearList();

    // Place the element back in the document for the next time we need it.
    this.element.hidden = true;
    this.chomeDocument.getElementById("mainPopupSet").appendChild(this.element);
  },

  /**
   * User-editable string used to filter the list of all logins.
   */
  filterString: "",

  /**
   * Handles text changes in the filter textbox.
   */
  onFilterInput() {
    this.filterString = this.filter.value;
    this.refreshList();
  },

  /**
   * Rebuilds the list of logins.
   */
  refreshList() {
    this.clearList();

    let formLogins = Services.logins.findLogins({}, "", "", null);
    let filterToUse = this.filterString.trim().toLowerCase();
    if (filterToUse) {
      formLogins = formLogins.filter(login => {
        return login.hostname.toLowerCase().indexOf(filterToUse) != -1 ||
               login.username.toLowerCase().indexOf(filterToUse) != -1 ;
      });
    }

    for (let { hostname, username } of formLogins) {
      let item = this.chomeDocument.createElementNS(XUL_NS, "richlistitem");
      item.classList.add("login-fill-item");
      item.setAttribute("hostname", hostname);
      item.setAttribute("username", username);
      this.list.appendChild(item);
    }
  },

  /**
   * Clears the list of logins.
   */
  clearList() {
    while (this.list.firstChild) {
      this.list.removeChild(this.list.firstChild);
    }
  },

  /**
   * Handles the action associated to a login item.
   */
  onListDblClick(event) {
    if (event.button != 0 || !this.list.selectedItem) {
      return;
    }
    this.fillLogin();
  },
  onListKeyPress(event) {
    if (event.keyCode != Ci.nsIDOMKeyEvent.DOM_VK_RETURN ||
        !this.list.selectedItem) {
      return;
    }
    this.fillLogin();
  },
  fillLogin() {
    this.hide();
  },
};

/**
 * Retrieves an existing FillDoorhanger associated with a browser, or null if an
 * associated doorhanger of that type cannot be found.
 *
 * @param An object with the following properties:
 *        {
 *          browser:
 *            The <browser> element to which the doorhanger is associated.
 *        }
 */
this.LoginDoorhangers.FillDoorhanger.find = function ({ browser }) {
  let PopupNotifications = browser.ownerDocument.defaultView.PopupNotifications;
  let notification = PopupNotifications.getNotification("password-fill",
                                                        browser);
  return notification && notification.doorhanger;
};
