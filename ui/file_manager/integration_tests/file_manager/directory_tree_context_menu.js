// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

'use strict';
(() => {
  // Filter used to wait for tree item to have fully loaded its children.
  const hasChildren = ' > .tree-row[has-children=true]';

  /**
   * Sets up for directory tree context menu test. In addition to normal setup,
   * we add destination directory.
   */
  async function setupForDirectoryTreeContextMenuTest() {
    const appId = await setupAndWaitUntilReady(RootPath.DOWNLOADS);

    // Add destination directory.
    await addEntries(['local'], [new TestEntryInfo({
                       type: EntryType.DIRECTORY,
                       targetPath: 'destination',
                       lastModifiedTime: 'Jan 1, 1980, 11:59 PM',
                       nameText: 'destination',
                       sizeText: '--',
                       typeText: 'Folder'
                     })]);
    return appId;
  }

  /**
   * @const
   */
  const ITEMS_IN_DEST_DIR_BEFORE_PASTE = TestEntryInfo.getExpectedRows([]);

  /**
   * @const
   */
  const ITEMS_IN_DEST_DIR_AFTER_PASTE =
      TestEntryInfo.getExpectedRows([new TestEntryInfo({
        type: EntryType.DIRECTORY,
        targetPath: 'photos',
        lastModifiedTime: 'Jan 1, 1980, 11:59 PM',
        nameText: 'photos',
        sizeText: '--',
        typeText: 'Folder'
      })]);

  /**
   * Clicks context menu item of id in directory tree.
   */
  async function clickDirectoryTreeContextMenuItem(appId, path, id) {
    const contextMenu = '#directory-tree-context-menu:not([hidden])';
    const pathQuery = `#directory-tree [full-path-for-testing="${path}"]`;

    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('focus', appId, [pathQuery]),
        'focus failed: ' + pathQuery);

    // Right click photos directory.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseRightClick', appId, [pathQuery]),
        'fakeMouseRightClick failed');

    // Check: context menu item |id| should be shown enabled.
    await remoteCall.waitForElement(
        appId, `${contextMenu} [command="#${id}"]:not([disabled])`);

    // Click the menu item specified by |id|.
    await remoteCall.callRemoteTestUtil(
        'fakeMouseClick', appId, [`${contextMenu} [command="#${id}"]`]);
  }

  /**
   * Navigates to destination directory and test paste operation to check
   * whether the paste operation is done correctly or not. This method does NOT
   * check source entry is deleted or not for cut operation.
   */
  async function navigateToDestinationDirectoryAndTestPaste(appId) {
    // Navigates to destination directory.
    await remoteCall.navigateWithDirectoryTree(
        appId, RootPath.DOWNLOADS_PATH + '/destination', 'My files/Downloads');

    // Confirm files before paste.
    await remoteCall.waitForFiles(
        appId, ITEMS_IN_DEST_DIR_BEFORE_PASTE, {ignoreLastModifiedTime: true});

    // Paste
    await remoteCall.callRemoteTestUtil(
        'fakeKeyDown', appId, ['body', 'v', true /* ctrl */, false, false]);

    // Confirm the photos directory is pasted correctly.
    await remoteCall.waitForFiles(
        appId, ITEMS_IN_DEST_DIR_AFTER_PASTE, {ignoreLastModifiedTime: true});
  }

  /**
   * Rename photos directory to specified name by using directory tree.
   */
  async function renamePhotosDirectoryTo(appId, newName, useKeyboardShortcut) {
    if (useKeyboardShortcut) {
      chrome.test.assertTrue(await remoteCall.callRemoteTestUtil(
          'fakeKeyDown', appId,
          ['body', 'Enter', true /* ctrl */, false, false]));
    } else {
      await clickDirectoryTreeContextMenuItem(
          appId, RootPath.DOWNLOADS_PATH + '/photos', 'rename');
    }
    await remoteCall.waitForElement(appId, '.tree-row > input');
    await remoteCall.callRemoteTestUtil(
        'inputText', appId, ['.tree-row > input', newName]);
    await remoteCall.callRemoteTestUtil(
        'fakeKeyDown', appId,
        ['.tree-row > input', 'Enter', false, false, false]);
  }

  /**
   * Renames directory and confirm current directory is moved to the renamed
   * directory.
   */
  async function renameDirectoryFromDirectoryTreeSuccessCase(
      useKeyboardShortcut) {
    const appId = await setupForDirectoryTreeContextMenuTest();

    await remoteCall.navigateWithDirectoryTree(
        appId, RootPath.DOWNLOADS_PATH + '/photos', 'My files/Downloads');
    await renamePhotosDirectoryTo(appId, 'New photos', useKeyboardShortcut);

    // Confirm that current directory has moved to new folder.
    await remoteCall.waitUntilCurrentDirectoryIsChanged(
        appId, '/My files/Downloads/New photos');
  }

  /**
   * Renames directory and confirms that an alert dialog is shown.
   */
  async function renameDirectoryFromDirectoryTreeAndConfirmAlertDialog(
      newName) {
    const appId = await setupForDirectoryTreeContextMenuTest();

    await remoteCall.navigateWithDirectoryTree(
        appId, RootPath.DOWNLOADS_PATH + '/photos', 'My files/Downloads');
    await renamePhotosDirectoryTo(appId, newName, false);

    // Confirm that a dialog is shown.
    await remoteCall.waitForElement(appId, '.cr-dialog-container.shown');
  }

  /**
   * Creates directory from directory tree.
   */
  async function createDirectoryFromDirectoryTree(
      useKeyboardShortcut, changeCurrentDirectory) {
    const appId = await setupForDirectoryTreeContextMenuTest();

    if (changeCurrentDirectory) {
      await remoteCall.navigateWithDirectoryTree(
          appId, RootPath.DOWNLOADS_PATH + '/photos', 'My files/Downloads');
    } else {
      const downloadsQuery =
          '#directory-tree [entry-label="My files"] [entry-label="Downloads"]';
      await remoteCall.expandTreeItemInDirectoryTree(appId, downloadsQuery);
    }
    if (useKeyboardShortcut) {
      await remoteCall.callRemoteTestUtil(
          'fakeKeyDown', appId, ['body', 'e', true /* ctrl */, false, false]);
    } else {
      await clickDirectoryTreeContextMenuItem(
          appId, RootPath.DOWNLOADS_PATH + '/photos', 'new-folder');
    }
    await remoteCall.waitForElement(appId, '.tree-row > input');
    await remoteCall.callRemoteTestUtil(
        'inputText', appId, ['.tree-row > input', 'test']);
    await remoteCall.callRemoteTestUtil(
        'fakeKeyDown', appId,
        ['.tree-row > input', 'Enter', false, false, false]);

    // Confirm that new directory is added to the directory tree.
    await remoteCall.waitForElement(
        appId,
        `[full-path-for-testing="${RootPath.DOWNLOADS_PATH}/photos/test"]`);

    // Confirm that current directory is not changed at this timing.
    await remoteCall.waitUntilCurrentDirectoryIsChanged(
        appId,
        changeCurrentDirectory ? '/My files/Downloads/photos' :
                                 '/My files/Downloads');

    // Confirm that new directory is actually created by navigating to it.
    await remoteCall.navigateWithDirectoryTree(
        appId, RootPath.DOWNLOADS_PATH + '/photos/test', 'My files/Downloads');
  }

  /**
   * Checks all visible items in the context menu for directory tree.
   * @param {!string} appId
   * @param {!string} breadcrumbsPath Path based on the entry labels like:
   *     /My files/Downloads/photos to item to be tested with context menu.
   * @param {!Array<!Array<string|boolean>>} menuStates Mapping each command to
   *     it's enabled state.
   * @param {boolean=} rootsMenu True if the item uses #roots-context-menu
   *     instead of #directory-tree-context-menu
   * @param {string=} shortcutToPath For shortcuts it navigates to a different
   *   breadcrumbs path, like /My Drive/ShortcutName.
   */
  async function checkContextMenu(
      appId, breadcrumbsPath, menuStates, rootsMenu, shortcutToPath) {
    // Focus the directory tree.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'focus', appId, ['#directory-tree']),
        'focus failed: #directory-tree');

    const paths = breadcrumbsPath.split('/').filter(path => path);
    const leaf = paths.pop();

    // Expand all parents of the leaf entry.
    let query = '#directory-tree';
    for (const parentLabel of paths) {
      query += ` [entry-label="${parentLabel}"]`;
      // Wait for parent element to be displayed.
      await remoteCall.waitForElement(appId, query);

      // Only expand if element isn't expanded yet.
      const elements = await remoteCall.callRemoteTestUtil(
          'queryAllElements', appId, [query + '[expanded]']);
      if (!elements.length) {
        await remoteCall.waitForElement(appId, query + hasChildren);
        await expandTreeItem(appId, query);
      }
    }

    // Wait for the final entry to be displayed.
    query += ` [entry-label="${leaf}"]`;
    await remoteCall.waitForElement(appId, query);

    // Navigate to the final entry.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeMouseClick', appId, [query]),
        'fakeMouseClick failed');
    // Wait to navigation to final entry to finish.
    await remoteCall.waitUntilCurrentDirectoryIsChanged(
        appId, (shortcutToPath || breadcrumbsPath));

    // Selector for a both context menu used on directory tree, only one should
    // be visible at the time.
    const menuQuery = rootsMenu ?
        '#roots-context-menu:not([hidden]) cr-menu-item:not([hidden])' :
        '#directory-tree-context-menu:not([hidden]) cr-menu-item:not([hidden])';

    // Right click desired item in the directory tree.
    await remoteCall.waitForElement(appId, query);
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseRightClick', appId, [query]),
        'fakeMouseRightClick failed');

    // Wait for context menu to appear.
    await remoteCall.waitForElement(appId, menuQuery);

    function stateString(state) {
      return state ? 'enabled' : 'disabled';
    }

    let msg;
    async function isCommandsEnabledAndOrdered() {
      // Grab all commands together and check they are in the expected order and
      // state.
      const actualItems = await remoteCall.callRemoteTestUtil(
          'queryAllElements', appId, [menuQuery]);
      let correctCommands = true;
      msg = '\nContext menu in the wrong order/state for: ' + breadcrumbsPath;
      for (let i = 0; i < Math.max(menuStates.length, actualItems.length);
           i++) {
        let expectedCommand = undefined;
        let expectedState = undefined;
        let actualCommand = undefined;
        let actualState = undefined;
        if (menuStates[i]) {
          expectedCommand = menuStates[i][0];
          expectedState = menuStates[i][1];
        }
        if (actualItems[i]) {
          actualCommand = actualItems[i].attributes['command'];
          actualState = actualItems[i].attributes['disabled'] ? false : true;
        }
        msg += '\n';
        if (expectedCommand !== actualCommand ||
            expectedState !== actualState) {
          correctCommands = false;
        }
        msg += ` index: ${i}`;
        msg +=
            `\n\t expected: ${expectedCommand} ${stateString(expectedState)}`;
        msg += `\n\t      got: ${actualCommand} ${stateString(actualState)}`;
      }
      return correctCommands;
    }

    // Check if commands are the way we expect, otherwise we try one more time.
    if (await isCommandsEnabledAndOrdered()) {
      return;
    }
    console.warn('Menus items did not match (trying again)...\n' + msg);
    await wait(REPEAT_UNTIL_INTERVAL);

    // Try the context menu one more time.
    await remoteCall.waitForElement(appId, query);
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseRightClick', appId, [query]),
        'fakeMouseRightClick failed');

    // Wait for context menu to appear.
    await remoteCall.waitForElement(appId, menuQuery);

    if (!await isCommandsEnabledAndOrdered()) {
      chrome.test.assertTrue(false, msg);
    }
  }

  /**
   * Tests copying a directory from directory tree with context menu.
   */
  testcase.dirCopyWithContextMenu = async () => {
    const appId = await setupForDirectoryTreeContextMenuTest();
    await remoteCall.navigateWithDirectoryTree(
        appId, RootPath.DOWNLOADS_PATH + '/photos', 'My files/Downloads');
    await clickDirectoryTreeContextMenuItem(
        appId, RootPath.DOWNLOADS_PATH + '/photos', 'copy');
    await navigateToDestinationDirectoryAndTestPaste(appId);
  };

  /**
   * Tests copying a directory from directory tree with the keyboard shortcut.
   */
  testcase.dirCopyWithKeyboard = async () => {
    const appId = await setupForDirectoryTreeContextMenuTest();
    await remoteCall.navigateWithDirectoryTree(
        appId, RootPath.DOWNLOADS_PATH + '/photos', 'My files/Downloads');

    // Press Ctrl+C.
    await remoteCall.callRemoteTestUtil(
        'fakeKeyDown', appId, ['body', 'c', true /* ctrl */, false, false]);
    await navigateToDestinationDirectoryAndTestPaste(appId);
  };

  /**
   * Tests copying a directory without changing the current directory.
   */
  testcase.dirCopyWithoutChangingCurrent = async () => {
    const appId = await setupForDirectoryTreeContextMenuTest();

    const downloadsQuery =
        '#directory-tree [entry-label="My files"] [entry-label="Downloads"]';
    await remoteCall.expandTreeItemInDirectoryTree(appId, downloadsQuery);
    await clickDirectoryTreeContextMenuItem(
        appId, RootPath.DOWNLOADS_PATH + '/photos', 'copy');
    await navigateToDestinationDirectoryAndTestPaste(appId);
  };

  /**
   * Tests cutting a directory with the context menu.
   */
  testcase.dirCutWithContextMenu = async () => {
    const appId = await setupForDirectoryTreeContextMenuTest();
    await remoteCall.navigateWithDirectoryTree(
        appId, RootPath.DOWNLOADS_PATH + '/photos', 'My files/Downloads');
    await clickDirectoryTreeContextMenuItem(
        appId, RootPath.DOWNLOADS_PATH + '/photos', 'cut');
    await navigateToDestinationDirectoryAndTestPaste(appId);

    // Confirm that directory tree is updated.
    await remoteCall.waitForElementLost(
        appId, `[full-path-for-testing="${RootPath.DOWNLOADS_PATH}/photos"]`);
  };

  /**
   * Tests cutting a directory with the keyboard shortcut.
   */
  testcase.dirCutWithKeyboard = async () => {
    const appId = await setupForDirectoryTreeContextMenuTest();
    await remoteCall.navigateWithDirectoryTree(
        appId, RootPath.DOWNLOADS_PATH + '/photos', 'My files/Downloads');

    // Press Ctrl+X.
    await remoteCall.callRemoteTestUtil(
        'fakeKeyDown', appId, ['body', 'x', true /* ctrl */, false, false]);
    await navigateToDestinationDirectoryAndTestPaste(appId);

    // Confirm that directory tree is updated.
    await remoteCall.waitForElementLost(
        appId, `[full-path-for-testing="${RootPath.DOWNLOADS_PATH}/photos"]`);
  };

  /**
   * Tests cutting a directory without changing the current directory.
   */
  testcase.dirCutWithoutChangingCurrent = async () => {
    const appId = await setupForDirectoryTreeContextMenuTest();

    const downloadsQuery =
        '#directory-tree [entry-label="My files"] [entry-label="Downloads"]';
    await remoteCall.expandTreeItemInDirectoryTree(appId, downloadsQuery);
    await clickDirectoryTreeContextMenuItem(
        appId, RootPath.DOWNLOADS_PATH + '/photos', 'cut');
    await navigateToDestinationDirectoryAndTestPaste(appId);
    await remoteCall.waitForElementLost(
        appId, `[full-path-for-testing="${RootPath.DOWNLOADS_PATH}/photos"]`);
  };

  /**
   * Tests pasting into folder with the context menu.
   */
  testcase.dirPasteWithContextMenu = async () => {
    const appId = await setupForDirectoryTreeContextMenuTest();
    const destinationPath = RootPath.DOWNLOADS_PATH + '/destination';

    // Copy photos directory as a test data.
    await remoteCall.navigateWithDirectoryTree(
        appId, RootPath.DOWNLOADS_PATH + '/photos', 'My files/Downloads');
    await remoteCall.callRemoteTestUtil(
        'fakeKeyDown', appId, ['body', 'c', true /* ctrl */, false, false]);
    await remoteCall.navigateWithDirectoryTree(
        appId, destinationPath, 'My files/Downloads');

    // Confirm files before paste.
    await remoteCall.waitForFiles(
        appId, ITEMS_IN_DEST_DIR_BEFORE_PASTE, {ignoreLastModifiedTime: true});

    await clickDirectoryTreeContextMenuItem(
        appId, destinationPath, 'paste-into-folder');

    // Confirm the photos directory is pasted correctly.
    await remoteCall.waitForFiles(
        appId, ITEMS_IN_DEST_DIR_AFTER_PASTE, {ignoreLastModifiedTime: true});

    // Expand the directory tree.
    await remoteCall.waitForElement(
        appId, `[full-path-for-testing="${destinationPath}"] .expand-icon`);
    await remoteCall.callRemoteTestUtil(
        'fakeMouseClick', appId,
        [`[full-path-for-testing="${destinationPath}"] .expand-icon`]);

    // Confirm the copied directory is added to the directory tree.
    await remoteCall.waitForElement(
        appId, `[full-path-for-testing="${destinationPath}/photos"]`);
  };

  /**
   * Tests pasting into a folder without changing the current directory.
   */
  testcase.dirPasteWithoutChangingCurrent = async () => {
    const destinationPath = RootPath.DOWNLOADS_PATH + '/destination';
    const downloadsQuery =
        '#directory-tree [entry-label="My files"] [entry-label="Downloads"]';

    const appId = await setupForDirectoryTreeContextMenuTest();
    await remoteCall.expandTreeItemInDirectoryTree(appId, downloadsQuery);
    await remoteCall.callRemoteTestUtil('focus', appId, ['#directory-tree']);
    await clickDirectoryTreeContextMenuItem(
        appId, RootPath.DOWNLOADS_PATH + '/photos', 'copy');
    await clickDirectoryTreeContextMenuItem(
        appId, destinationPath, 'paste-into-folder');
    await remoteCall.waitForElement(
        appId,
        `[full-path-for-testing="${destinationPath}"][may-have-children]`);
    await remoteCall.callRemoteTestUtil(
        'fakeMouseClick', appId,
        [`[full-path-for-testing="${destinationPath}"] .expand-icon`]);

    // Confirm the copied directory is added to the directory tree.
    await remoteCall.waitForElement(
        appId, `[full-path-for-testing="${destinationPath}/photos"]`);
  };

  /**
   * Tests renaming a folder with the context menu.
   */
  testcase.dirRenameWithContextMenu = () => {
    return renameDirectoryFromDirectoryTreeSuccessCase(
        false /* do not use keyboard shortcut */);
  };

  /**
   * Tests that a child folder breadcrumbs is updated when renaming its parent
   * folder. crbug.com/885328.
   */
  testcase.dirRenameUpdateChildrenBreadcrumbs = async () => {
    const appId = await setupAndWaitUntilReady(RootPath.DOWNLOADS);

    // Add child-folder inside /photos/
    await addEntries(['local'], [new TestEntryInfo({
                       type: EntryType.DIRECTORY,
                       targetPath: 'photos/child-folder',
                       lastModifiedTime: 'Jan 1, 1980, 11:59 PM',
                       nameText: 'child-folder',
                       sizeText: '--',
                       typeText: 'Folder'
                     })]);

    // Navigate to child folder.
    await remoteCall.navigateWithDirectoryTree(
        appId, RootPath.DOWNLOADS_PATH + '/photos/child-folder',
        'My files/Downloads');

    // Rename parent folder.
    await clickDirectoryTreeContextMenuItem(
        appId, RootPath.DOWNLOADS_PATH + '/photos', 'rename');
    await remoteCall.waitForElement(appId, '.tree-row > input');
    await remoteCall.callRemoteTestUtil(
        'inputText', appId, ['.tree-row > input', 'photos-new']);
    const enterKey = ['.tree-row > input', 'Enter', false, false, false];
    chrome.test.assertTrue(
        await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, enterKey),
        'Enter key failed');

    // Confirm that current directory is now My files or /Downloads, because it
    // can't find the previously selected folder /Downloads/photos/child-folder,
    // since its path/parent has been renamed.
    // TODO(lucmult): Remove this conditional once MyFilesVolume is rolled out.
    await remoteCall.waitUntilCurrentDirectoryIsChanged(
        appId,
        RootPath.DOWNLOADS_PATH === '/Downloads' ? '/My files' :
                                                   '/My files/Downloads');

    // Navigate to child-folder using the new path.
    // |navigateWithDirectoryTree| already checks for breadcrumbs to
    // match the path.
    await remoteCall.navigateWithDirectoryTree(
        appId, RootPath.DOWNLOADS_PATH + '/photos-new/child-folder',
        'My files/Downloads');
  };

  /**
   * Tests renaming folder with the keyboard shortcut.
   */
  testcase.dirRenameWithKeyboard = () => {
    return renameDirectoryFromDirectoryTreeSuccessCase(
        true /* use keyboard shortcut */);
  };

  /**
   * Tests renaming folder without changing the current directory.
   */
  testcase.dirRenameWithoutChangingCurrent = async () => {
    const appId = await setupForDirectoryTreeContextMenuTest();
    const downloadsQuery =
        '#directory-tree [entry-label="My files"] [entry-label="Downloads"]';
    await remoteCall.expandTreeItemInDirectoryTree(appId, downloadsQuery);
    await remoteCall.waitForElement(
        appId, `[full-path-for-testing="${RootPath.DOWNLOADS_PATH}/photos"]`);
    await renamePhotosDirectoryTo(
        appId, 'New photos', false /* Do not use keyboard shortcut. */);
    await remoteCall.waitForElementLost(
        appId, `[full-path-for-testing="${RootPath.DOWNLOADS_PATH}/photos"]`);
    await remoteCall.waitForElement(
        appId,
        `[full-path-for-testing="${RootPath.DOWNLOADS_PATH}/New photos"]`);
  };

  /**
   * Tests renaming a folder to an empty string.
   */
  testcase.dirRenameToEmptyString = async () => {
    const appId = await setupForDirectoryTreeContextMenuTest();

    await remoteCall.navigateWithDirectoryTree(
        appId, RootPath.DOWNLOADS_PATH + '/photos', 'My files/Downloads');
    await renamePhotosDirectoryTo(appId, '', false);

    // Wait for the input to be removed.
    await remoteCall.waitForElementLost(appId, '.tree-row > input');

    // No dialog should be shown.
    await remoteCall.waitForElementLost(appId, '.cr-dialog-container.shown');
  };

  /**
   * Tests renaming folder an existing name.
   */
  testcase.dirRenameToExisting = () => {
    return renameDirectoryFromDirectoryTreeAndConfirmAlertDialog('destination');
  };

  /**
   * Tests creating a folder with the context menu.
   */
  testcase.dirCreateWithContextMenu = () => {
    return createDirectoryFromDirectoryTree(
        false /* do not use keyboard shortcut */,
        true /* change current directory */);
  };

  /**
   * Tests creating a folder with the keyboard shortcut.
   */
  testcase.dirCreateWithKeyboard = () => {
    return createDirectoryFromDirectoryTree(
        true /* use keyboard shortcut */, true /* change current directory */);
  };

  /**
   * Tests creating folder without changing the current directory.
   */
  testcase.dirCreateWithoutChangingCurrent = () => {
    return createDirectoryFromDirectoryTree(
        false /* Do not use keyboard shortcut */,
        false /* Do not change current directory */);
  };

  /**
   * Tests context menu for Recent root, currently it doesn't show context menu.
   */
  testcase.dirContextMenuRecent = async () => {
    const query = '#directory-tree [dir-type="FakeItem"][entry-label="Recent"]';

    // Open Files app on Downloads.
    const appId = await setupAndWaitUntilReady(RootPath.DOWNLOADS);

    // Focus the directory tree.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'focus', appId, ['#directory-tree']),
        'focus failed: #directory-tree');

    // Select Recent root.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeMouseClick', appId, [query]),
        'fakeMouseClick failed');

    // Wait it to navigate to it.
    await remoteCall.waitUntilCurrentDirectoryIsChanged(appId, '/Recent');

    // Right click Recent root.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseRightClick', appId, [query]),
        'fakeMouseRightClick failed');

    // Check that both menus are still hidden.
    await remoteCall.waitForElement(appId, '#roots-context-menu[hidden]');
    await remoteCall.waitForElement(
        appId, '#directory-tree-context-menu[hidden]');
  };

  /**
   * Tests context menu for a Zip root and a folder inside it.
   */
  testcase.dirContextMenuZip = async () => {
    const zipMenus = [
      ['#unmount', true],
    ];
    const folderMenus = [
      ['#cut', false],
      ['#copy', true],
      ['#paste-into-folder', false],
      ['#rename', false],
      ['#delete', false],
      ['#new-folder', false],
    ];

    // Open Files app on Downloads containing a zip file.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.zipArchive], []);

    // Select the zip file.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'selectFile', appId, ['archive.zip']),
        'selectFile failed');

    // Press the Enter key to mount the zip file.
    const key = ['#file-list', 'Enter', false, false, false];
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeKeyDown', appId, key),
        'fakeKeyDown failed');

    // Check the context menu is on desired state.
    await checkContextMenu(
        appId, '/archive.zip', zipMenus, true /* rootMenu */);

    // Check the context menu for a folder inside the zip.
    await checkContextMenu(
        appId, '/archive.zip/folder', folderMenus, false /* rootMenu */);
  };

  /**
   * Tests context menu for Shortcut roots.
   */
  testcase.dirContextMenuShortcut = async () => {
    const menus = [
      ['#rename', false],
      ['#remove-folder-shortcut', true],
      ['#share-with-linux', true],
    ];
    const entry = ENTRIES.directoryD;
    const entryName = entry.nameText;

    // Open Files app on Drive.
    const appId = await setupAndWaitUntilReady(RootPath.DRIVE, [], [entry]);

    // Create a shortcut to directory D.
    await createShortcut(appId, entryName);

    // Check the context menu is on desired state.
    await checkContextMenu(
        appId, `/${entryName}`, menus, true /* rootMenu */,
        `/My Drive/${entryName}`);
  };

  /**
   * Tests context menu for MyFiles, Downloads and sub-folder.
   */
  testcase.dirContextMenuMyFiles = async () => {
    const myFilesMenus = [
      ['#share-with-linux', true],
      ['#new-folder', true],
    ];
    const downloadsMenus = [
      ['#cut', false],
      ['#copy', true],
      ['#paste-into-folder', false],
      ['#share-with-linux', true],
      ['#delete', false],
      ['#new-folder', true],
    ];
    const photosMenus = [
      ['#cut', true],
      ['#copy', true],
      ['#paste-into-folder', false],
      ['#share-with-linux', true],
      ['#rename', true],
      ['#delete', true],
      ['#new-folder', true],
    ];

    // Open Files app on local Downloads.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.beautiful, ENTRIES.photos], []);

    // Check the context menu is on desired state for MyFiles.
    await checkContextMenu(
        appId, '/My files', myFilesMenus, false /* rootMenu */);

    // Check the context menu for MyFiles>Downloads.
    await checkContextMenu(
        appId, '/My files/Downloads', downloadsMenus, false /* rootMenu */);

    // Check the context menu for MyFiles>Downloads>photos.
    await checkContextMenu(
        appId, '/My files/Downloads/photos', photosMenus, false /* rootMenu */);
  };

  /**
   * Tests context menu for Crostini real root and a folder inside it.
   */
  testcase.dirContextMenuCrostini = async () => {
    const linuxMenus = [
      ['#new-folder', true],
    ];
    const folderMenus = [
      ['#cut', true],
      ['#copy', true],
      ['#paste-into-folder', false],
      ['#rename', true],
      ['#delete', true],
      ['#new-folder', true],
    ];
    const linuxQuery = '#directory-tree [entry-label="Linux files"]';

    // Add a crostini folder.
    await addEntries(['crostini'], [ENTRIES.photos]);

    // Open Files app on local Downloads.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.beautiful], []);

    // Select Crostini, because the first right click doesn't show any context
    // menu, just actually mounts crostini converting the tree item from fake to
    // real root.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseClick', appId, [linuxQuery]),
        'fakeMouseClick failed');

    // Wait for the real root to appear.
    await remoteCall.waitForElement(
        appId,
        '#directory-tree ' +
            '[dir-type="SubDirectoryItem"][entry-label="Linux files"]');

    // Check the context menu for Linux files.
    await checkContextMenu(
        appId, '/My files/Linux files', linuxMenus, false /* rootMenu */);

    // Check the context menu for a folder in Linux files.
    await checkContextMenu(
        appId, '/My files/Linux files/photos', folderMenus,
        false /* rootMenu */);
  };

  /**
   * Tests context menu for ARC++/Play files root and a folder inside it.
   */
  testcase.dirContextMenuPlayFiles = async () => {
    const playFilesMenus = [
      ['#share-with-linux', true],
      ['#new-folder', false],
    ];
    const folderMenus = [
      ['#cut', true],
      ['#copy', true],
      ['#paste-into-folder', false],
      ['#share-with-linux', true],
      ['#rename', false],
      ['#delete', true],
      ['#new-folder', true],
    ];

    // Add an Android folder.
    await addEntries(['android_files'], [ENTRIES.directoryDocuments]);

    // Open Files app on local Downloads.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.beautiful], []);

    // Check the context menu for Play files.
    await checkContextMenu(
        appId, '/My files/Play files', playFilesMenus, false /* rootMenu */);

    // Check the context menu for a folder in Play files.
    await checkContextMenu(
        appId, '/My files/Play files/Documents', folderMenus,
        false /* rootMenu */);
  };

  /**
   * Tests context menu for USB root (single and multiple partitions).
   */
  testcase.dirContextMenuUsbs = async () => {
    const singleUsbMenus = [
      ['#unmount', true],
      ['#format', true],
      ['#rename', false],
      ['#share-with-linux', true],
    ];
    const partitionsRootMenus = [
      ['#unmount', true],
      ['#format', false],
      ['#share-with-linux', true],
    ];
    const partition1Menus = [
      ['#share-with-linux', true],
      ['#format', true],
      ['#rename', false],
      ['#new-folder', true],
    ];
    const folderMenus = [
      ['#cut', true],
      ['#copy', true],
      ['#paste-into-folder', false],
      ['#share-with-linux', true],
      ['#rename', true],
      ['#delete', true],
      ['#new-folder', true],
    ];

    // Mount removable volumes.
    await sendTestMessage({name: 'mountUsbWithPartitions'});
    await sendTestMessage({name: 'mountFakeUsb'});

    // Open Files app on local Downloads.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.beautiful], []);

    // Check the context menu for single partition USB.
    await checkContextMenu(
        appId, '/fake-usb', singleUsbMenus, true /* rootMenu */);

    // Check the context menu for a folder inside a singlue USB partition.
    await checkContextMenu(
        appId, '/fake-usb/A', folderMenus, false /* rootMenu */);

    // Check the context menu for multiple partitions USB (root).
    await checkContextMenu(
        appId, '/Drive Label', partitionsRootMenus, true /* rootMenu */);

    // Check the context menu for multiple partitions USB (actual partition).
    await checkContextMenu(
        appId, '/Drive Label/partition-1', partition1Menus,
        false /* rootMenu */);

    // Check the context menu for a folder inside a partition1.
    await checkContextMenu(
        appId, '/Drive Label/partition-1/Folder', folderMenus,
        false /* rootMenu */);
  };

  /**
   * Tests context menu for USB root with DCIM folder.
   */
  testcase.dirContextMenuUsbDcim = async () => {
    const usbMenus = [
      ['#unmount', true],
      ['#format', true],
      ['#rename', false],
      ['#share-with-linux', true],
    ];
    const dcimFolderMenus = [
      ['#cut', true],
      ['#copy', true],
      ['#paste-into-folder', false],
      ['#share-with-linux', true],
      ['#rename', true],
      ['#delete', true],
      ['#new-folder', true],
    ];

    // Mount removable volumes.
    await sendTestMessage({name: 'mountFakeUsbDcim'});

    // Open Files app on local Downloads.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.beautiful], []);

    // Check the context menu for single partition USB.
    await checkContextMenu(appId, '/fake-usb', usbMenus, true /* rootMenu */);

    // Check the context menu for the DCIM folder inside USB.
    await checkContextMenu(
        appId, '/fake-usb/DCIM', dcimFolderMenus, false /* rootMenu */);
  };

  /*
   * Tests context menu for Mtp root and a folder inside it.
   */
  testcase.dirContextMenuMtp = async () => {
    const folderMenus = [
      ['#cut', true],
      ['#copy', true],
      ['#paste-into-folder', false],
      ['#rename', true],
      ['#delete', true],
      ['#new-folder', true],
    ];

    const mtpQuery = '#directory-tree [entry-label="fake-mtp"]';
    const folderQuery = mtpQuery + ' [entry-label="A"]';

    // Mount fake MTP volume.
    await sendTestMessage({name: 'mountFakeMtp'});

    // Open Files app on local Downloads.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.beautiful], []);

    // Select Recent root.
    await remoteCall.waitAndClickElement(appId, mtpQuery + hasChildren);

    // Right click on MTP root.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseRightClick', appId, [mtpQuery]),
        'fakeMouseRightClick failed');

    // Check that both menus are still hidden, since there is not context menu
    // for MTP root.
    await remoteCall.waitForElement(appId, '#roots-context-menu[hidden]');
    await remoteCall.waitForElement(
        appId, '#directory-tree-context-menu[hidden]');

    // Check the context menu for a folder inside a MTP.
    await checkContextMenu(
        appId, '/fake-mtp/A', folderMenus, false /* rootMenu */);
  };

  /**
   * Tests context menu for FSP root and a folder inside it.
   */
  testcase.dirContextMenuFsp = async () => {
    const fspMenus = [
      ['#unmount', true],
    ];
    const folderMenus = [
      ['#cut', false],
      ['#copy', true],
      ['#paste-into-folder', false],
      ['#rename', false],
      ['#delete', false],
      ['#new-folder', false],
    ];

    // Install a FSP.
    const manifest = 'manifest_source_file.json';
    await sendTestMessage(
        {name: 'launchProviderExtension', manifest: manifest});

    // Open Files app on local Downloads.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.beautiful], []);

    // Check the context menu for FSP root.
    await checkContextMenu(appId, '/Test (1)', fspMenus, true /* rootMenu */);

    // Check the context menu for a folder inside a FSP.
    await checkContextMenu(
        appId, '/Test (1)/folder', folderMenus, false /* rootMenu */);
  };

  /**
   * Tests context menu for DocumentsProvider root and a folder inside it.
   */
  testcase.dirContextMenuDocumentsProvider = async () => {
    const folderMenus = [
      ['#cut', false],
      ['#copy', true],
      ['#paste-into-folder', false],
      ['#rename', false],
      ['#delete', false],
      ['#new-folder', false],
    ];
    const documentsProviderQuery =
        '#directory-tree [entry-label="DocumentsProvider"]';

    // Add a DocumentsProvider folder.
    await addEntries(['documents_provider'], [ENTRIES.photos]);

    // Open Files app on local Downloads.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.beautiful], []);

    // Wait for DocumentsProvider to appear.
    await remoteCall.waitForElement(
        appId, documentsProviderQuery + hasChildren);

    // Select DocumentsProvider root.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseClick', appId, [documentsProviderQuery]),
        'fakeMouseClick failed');

    // Wait it to navigate to it.
    await remoteCall.waitUntilCurrentDirectoryIsChanged(
        appId, '/DocumentsProvider');

    // Right click DocumentsProvider root.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseRightClick', appId, [documentsProviderQuery]),
        'fakeMouseRightClick failed');

    // Check that both menus are still hidden, because DocumentsProvider root
    // doesn't show any context menu.
    await remoteCall.waitForElement(appId, '#roots-context-menu[hidden]');
    await remoteCall.waitForElement(
        appId, '#directory-tree-context-menu[hidden]');

    // Check the context menu for a folder inside a DocumentsProvider.
    await checkContextMenu(
        appId, '/DocumentsProvider/photos', folderMenus, false /* rootMenu */);
  };

  /**
   * Tests context menu for Audio, Images and Videos roots, currently they don't
   * show context menu.
   */
  testcase.dirContextMenuMediaView = async () => {
    const audioViewQuery = '#directory-tree [entry-label="Audio"]';
    const imagesViewQuery = '#directory-tree [entry-label="Images"]';
    const videosViewQuery = '#directory-tree [entry-label="Videos"]';

    await sendTestMessage({name: 'mountMediaView'});

    // Open Files app on local Downloads.
    const appId = await setupAndWaitUntilReady(
        RootPath.DOWNLOADS, [ENTRIES.beautiful], []);

    // Right click Audio root.
    await remoteCall.waitAndClickElement(appId, audioViewQuery);
    await remoteCall.waitUntilCurrentDirectoryIsChanged(appId, '/Audio');
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseRightClick', appId, [audioViewQuery]),
        'fakeMouseRightClick failed');

    // Check that both menus are still hidden.
    await remoteCall.waitForElement(appId, '#roots-context-menu[hidden]');
    await remoteCall.waitForElement(
        appId, '#directory-tree-context-menu[hidden]');

    // Right click Images root.
    await remoteCall.waitAndClickElement(appId, imagesViewQuery);
    await remoteCall.waitUntilCurrentDirectoryIsChanged(appId, '/Images');
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseRightClick', appId, [imagesViewQuery]),
        'fakeMouseRightClick failed');

    // Check that both menus are still hidden.
    await remoteCall.waitForElement(appId, '#roots-context-menu[hidden]');
    await remoteCall.waitForElement(
        appId, '#directory-tree-context-menu[hidden]');

    // Right click Videos root.
    await remoteCall.waitAndClickElement(appId, videosViewQuery);
    await remoteCall.waitUntilCurrentDirectoryIsChanged(appId, '/Videos');
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseRightClick', appId, [videosViewQuery]),
        'fakeMouseRightClick failed');

    // Check that both menus are still hidden.
    await remoteCall.waitForElement(appId, '#roots-context-menu[hidden]');
    await remoteCall.waitForElement(
        appId, '#directory-tree-context-menu[hidden]');
  };

  /**
   * Tests context menu for My Drive, read-only and read-write folder inside it.
   */
  testcase.dirContextMenuMyDrive = async () => {
    const myDriveMenus = [
      ['#share-with-linux', true],
      ['#new-folder', true],
    ];
    const readOnlyFolderMenus = [
      ['#cut', false],
      ['#copy', true],
      ['#paste-into-folder', false],
      ['#share-with-linux', true],
      ['#rename', false],
      ['#create-folder-shortcut', true],
      ['#delete', false],
      ['#new-folder', false],
    ];
    const readWriteFolderMenus = [
      ['#cut', true],
      ['#copy', true],
      ['#paste-into-folder', true],
      ['#share-with-linux', true],
      ['#rename', true],
      ['#create-folder-shortcut', true],
      ['#delete', true],
      ['#new-folder', true],
    ];

    // Open Files App on Drive.
    const appId = await setupAndWaitUntilReady(
        RootPath.DRIVE, [], COMPLEX_DRIVE_ENTRY_SET);

    // Select and copy hello.txt into the clipboard to test paste-into-folder
    // command.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'selectFile', appId, ['hello.txt']),
        'selectFile failed');
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('execCommand', appId, ['copy']),
        'execCommand failed');

    // Check that Google Drive is expanded.
    await remoteCall.waitForElement(
        appId, ['#directory-tree .tree-item.drive-volume[expanded]']);

    // Check the context menu for My Drive root.
    await checkContextMenu(
        appId, '/My Drive', myDriveMenus, false /* rootMenu */);

    // Check the context menu for read-only folder.
    await checkContextMenu(
        appId, '/My Drive/Read-Only Folder', readOnlyFolderMenus,
        false /* rootMenu */);

    // Check the context menu for read+write folder.
    await checkContextMenu(
        appId, '/My Drive/photos', readWriteFolderMenus, false /* rootMenu */);
  };

  /**
   * Tests context menu for Shared drives grand-root, a read+write shared drive
   * root, a folder inside it, a read-only shared drive and a folder inside
   * it.
   */
  testcase.dirContextMenuSharedDrive = async () => {
    const sharedDriveGrandRootMenus = [
      ['#share-with-linux', true],
    ];
    const readWriteSharedDriveRootMenus = [
      ['#cut', false],
      ['#copy', false],
      ['#paste-into-folder', true],
      ['#share-with-linux', true],
      ['#rename', false],
      ['#delete', true],
      ['#new-folder', true],
    ];
    const readWriteFolderMenus = [
      ['#cut', true],
      ['#copy', true],
      ['#paste-into-folder', true],
      ['#share-with-linux', true],
      ['#rename', true],
      ['#create-folder-shortcut', true],
      ['#delete', true],
      ['#new-folder', true],
    ];
    const readOnlySharedDriveRootMenus = [
      ['#cut', false],
      ['#copy', false],
      ['#paste-into-folder', false],
      ['#share-with-linux', true],
      ['#rename', false],
      ['#delete', false],
      ['#new-folder', false],
    ];
    const readOnlyFolderMenus = [
      ['#cut', false],
      ['#copy', true],
      ['#paste-into-folder', false],
      ['#share-with-linux', true],
      ['#rename', false],
      ['#create-folder-shortcut', true],
      ['#delete', false],
      ['#new-folder', false],
    ];

    // Open Files App on Drive.
    const appId = await setupAndWaitUntilReady(
        RootPath.DRIVE, [], SHARED_DRIVE_ENTRY_SET);

    // Select and copy hello.txt into the clipboard to test paste-into-folder
    // command.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'selectFile', appId, ['hello.txt']),
        'selectFile failed');
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('execCommand', appId, ['copy']),
        'execCommand failed');

    // Check that Google Drive is expanded.
    await remoteCall.waitForElement(
        appId, ['#directory-tree .tree-item.drive-volume[expanded]']);

    // Check the context menu for Shared drives grand root.
    await checkContextMenu(
        appId, '/Shared drives', sharedDriveGrandRootMenus,
        false /* rootMenu */);

    // Check the context menu for a read+write shared drive root.
    await checkContextMenu(
        appId, '/Shared drives/Team Drive A', readWriteSharedDriveRootMenus,
        false /* rootMenu */);

    // Check the context menu for read+write folder.
    await checkContextMenu(
        appId, '/Shared drives/Team Drive A/teamDriveADirectory',
        readWriteFolderMenus, false /* rootMenu */);

    // Check the context menu for a read-only shared drive root.
    await checkContextMenu(
        appId, '/Shared drives/Team Drive B', readOnlySharedDriveRootMenus,
        false /* rootMenu */);

    // Check the context menu for read+write folder.
    await checkContextMenu(
        appId, '/Shared drives/Team Drive B/teamDriveBDirectory',
        readOnlyFolderMenus, false /* rootMenu */);
  };

  /**
   * Tests context menu for Google Drive/Shared with me root, currently it
   * doesn't show context menu.
   */
  testcase.dirContextMenuSharedWithMe = async () => {
    const query = '#directory-tree [entry-label="Shared with me"]';

    // Open Files app on Drive.
    const appId =
        await setupAndWaitUntilReady(RootPath.DRIVE, [], BASIC_DRIVE_ENTRY_SET);

    // Focus the directory tree.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'focus', appId, ['#directory-tree']),
        'focus failed: #directory-tree');

    // Select Shared with me root.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeMouseClick', appId, [query]),
        'fakeMouseClick failed');

    // Wait it to navigate to it.
    await remoteCall.waitUntilCurrentDirectoryIsChanged(
        appId, '/Shared with me');

    // Right click Shared with me root.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseRightClick', appId, [query]),
        'fakeMouseRightClick failed');

    // Check that both menus are still hidden.
    await remoteCall.waitForElement(appId, '#roots-context-menu[hidden]');
    await remoteCall.waitForElement(
        appId, '#directory-tree-context-menu[hidden]');
  };

  /**
   * Tests context menu for Google Drive/Offline root, currently it doesn't show
   * context menu.
   */
  testcase.dirContextMenuOffline = async () => {
    const query = '#directory-tree [entry-label="Offline"]';

    // Open Files app on Drive.
    const appId =
        await setupAndWaitUntilReady(RootPath.DRIVE, [], BASIC_DRIVE_ENTRY_SET);

    // Focus the directory tree.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'focus', appId, ['#directory-tree']),
        'focus failed: #directory-tree');

    // Select Shared with me root.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('fakeMouseClick', appId, [query]),
        'fakeMouseClick failed');

    // Wait it to navigate to it.
    await remoteCall.waitUntilCurrentDirectoryIsChanged(appId, '/Offline');

    // Right click Shared with me root.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'fakeMouseRightClick', appId, [query]),
        'fakeMouseRightClick failed');

    // Check that both menus are still hidden.
    await remoteCall.waitForElement(appId, '#roots-context-menu[hidden]');
    await remoteCall.waitForElement(
        appId, '#directory-tree-context-menu[hidden]');
  };

  /**
   * Tests context menu for Google Drive/Computer grand-root, a computer root, a
   * folder inside it.
   */
  testcase.dirContextMenuComputers = async () => {
    const computersGrandRootMenus = [
      ['#cut', true],
      ['#copy', true],
      ['#paste-into-folder', false],
      ['#rename', true],
      ['#delete', false],
      ['#new-folder', false],
    ];
    const computerRootMenus = [
      ['#cut', true],
      ['#copy', true],
      ['#paste-into-folder', false],
      ['#rename', false],
      ['#delete', false],
      ['#new-folder', false],
    ];
    const folderMenus = [
      ['#cut', true],
      ['#copy', true],
      ['#paste-into-folder', true],
      ['#share-with-linux', true],
      ['#rename', false],
      ['#create-folder-shortcut', true],
      ['#delete', true],
      ['#new-folder', true],
    ];

    // Open Files App on Drive.
    const appId =
        await setupAndWaitUntilReady(RootPath.DRIVE, [], COMPUTERS_ENTRY_SET);

    // Select and copy hello.txt into the clipboard to test paste-into-folder
    // command.
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil(
            'selectFile', appId, ['hello.txt']),
        'selectFile failed');
    chrome.test.assertTrue(
        !!await remoteCall.callRemoteTestUtil('execCommand', appId, ['copy']),
        'execCommand failed');

    // Check that Google Drive is expanded.
    await remoteCall.waitForElement(
        appId, ['#directory-tree .tree-item.drive-volume[expanded]']);

    // Check the context menu for Computers grand root.
    await checkContextMenu(
        appId, '/Computers', computersGrandRootMenus, false /* rootMenu */);

    // Check the context menu for a computer root.
    await checkContextMenu(
        appId, '/Computers/Computer A', computerRootMenus,
        false /* rootMenu */);

    // Check the context menu for a folder inside a computer.
    await checkContextMenu(
        appId, '/Computers/Computer A/A', folderMenus, false /* rootMenu */);
  };
})();
