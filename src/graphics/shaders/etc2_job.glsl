// A job's nine words: the push constants, or for a batched dispatch the entry
// whose word offset is in table slot gl_WorkGroupID.z, read from `src`.
// Include after `src` is declared.
layout(push_constant) uniform Push {
  uint words[9];
  uint table[23];
} push;

// Push word 8 bit: a batched dispatch. The entry's own word 8 holds the job's
// flags.
const uint kBatchFlag = 2u;

bool Batched() { return (push.words[8] & kBatchFlag) != 0u; }

uint JobWord(uint i) {
  return Batched() ? src[push.table[gl_WorkGroupID.z] + i] : push.words[i];
}

// The layer a dispatch's z covers; batched jobs have one.
uint JobLayer() { return Batched() ? 0u : gl_GlobalInvocationID.z; }
