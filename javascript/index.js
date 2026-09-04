// ESM entry point. The implementation lives in core.cjs so that the same code
// serves both `import` and `require` without a build step.
import core from './core.cjs';

export const BitClient = core.BitClient;
export const BitError = core.BitError;
export default core;
