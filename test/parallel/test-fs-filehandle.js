// Flags: --expose-gc --no-warnings --expose-internals
'use strict';

const common = require('../common');
const assert = require('assert');
const path = require('path');
const { internalBinding } = require('internal/test/binding');
const fs = internalBinding('fs');
const { stringToFlags } = require('internal/fs/utils');

// Verifies that the FileHandle object is garbage collected and that a
// warning is emitted if it is not closed.

let fdnum;
{
  const ctx = {};
  fdnum = fs.openFileHandle(path.toNamespacedPath(__filename),
                            stringToFlags('r'), 0o666, undefined, ctx).fd;
  assert.strictEqual(ctx.errno, undefined);
}

common.expectWarning({
  'internal/test/binding': [
    'These APIs are exposed only for testing ' +
    'and are not tracked by any versioning system or deprecation process.',
    common.noWarnCode
  ],
  'Warning': [
    `Closing file descriptor ${fdnum} on garbage collection`,
    common.noWarnCode
  ]
});

global.gc();

setTimeout(() => {}, 10);
