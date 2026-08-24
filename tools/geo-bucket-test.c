// Standalone check of the geo_bucket_by_display_list() list surgery from
// src/game/rendering_graph_node.c. Verifies on randomised inputs that the
// result is a stable permutation with equal display lists made adjacent:
//   - same set of nodes, no loss, no duplication, no cycle
//   - each display list occupies exactly one contiguous run
//   - groups appear in order of first appearance in the input
//   - within a group, nodes keep their original relative order
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>

typedef unsigned long long u64;
typedef unsigned int u32;

struct DisplayListNode {
    const void *displayList;
    struct DisplayListNode *next;
    int seq;   // original index, for stability checking
};

#define GEO_BUCKET_SLOTS 512

static struct GeoBucket {
    const void *displayList;
    struct DisplayListNode *tail;
    u32 gen;
} sGeoBuckets[GEO_BUCKET_SLOTS];
static u32 sGeoBucketGen = 0;

static struct DisplayListNode *geo_bucket_by_display_list(struct DisplayListNode *list) {
    struct DisplayListNode *outHead = NULL;
    struct DisplayListNode *outTail = NULL;
    const u32 gen = ++sGeoBucketGen;

    for (struct DisplayListNode *n = list; n != NULL; ) {
        struct DisplayListNode *next = n->next;
        const void *dl = n->displayList;

        u64 h = (u64) (uintptr_t) dl >> 3;
        h *= 0x9E3779B97F4A7C15ULL;
        u32 i = (u32) (h >> 48) & (GEO_BUCKET_SLOTS - 1);

        u32 probes = 0;
        while (sGeoBuckets[i].gen == gen && sGeoBuckets[i].displayList != dl) {
            i = (i + 1) & (GEO_BUCKET_SLOTS - 1);
            if (++probes >= GEO_BUCKET_SLOTS) { break; }
        }

        bool tracked = (probes < GEO_BUCKET_SLOTS);
        if (tracked && sGeoBuckets[i].gen == gen) {
            struct DisplayListNode *tail = sGeoBuckets[i].tail;
            n->next = tail->next;
            tail->next = n;
            if (outTail == tail) { outTail = n; }
            sGeoBuckets[i].tail = n;
        } else {
            n->next = NULL;
            if (outTail != NULL) { outTail->next = n; } else { outHead = n; }
            outTail = n;
            if (tracked) {
                sGeoBuckets[i].gen = gen;
                sGeoBuckets[i].displayList = dl;
                sGeoBuckets[i].tail = n;
            }
        }
        n = next;
    }
    return outHead;
}

int main(void) {
    srand(12345);
    int failures = 0;

    for (int trial = 0; trial < 20000; trial++) {
        int n = rand() % 60;                      // list length, including empty
        int distinct = 1 + rand() % 8;            // how many display lists in play
        struct DisplayListNode *nodes = calloc(n ? n : 1, sizeof(*nodes));
        // Aligned fake pointers, like real Gfx*.
        static char pool[16][64];

        struct DisplayListNode *head = NULL, *tail = NULL;
        for (int i = 0; i < n; i++) {
            nodes[i].displayList = pool[rand() % distinct];
            nodes[i].seq = i;
            nodes[i].next = NULL;
            if (tail) { tail->next = &nodes[i]; } else { head = &nodes[i]; }
            tail = &nodes[i];
        }

        struct DisplayListNode *out = geo_bucket_by_display_list(head);

        // 1. same multiset of nodes, terminating (no cycle)
        int count = 0;
        int *seen = calloc(n ? n : 1, sizeof(int));
        for (struct DisplayListNode *p = out; p != NULL; p = p->next) {
            if (++count > n) { printf("trial %d: cycle or extra nodes\n", trial); failures++; break; }
            if (seen[p->seq]++) { printf("trial %d: node %d twice\n", trial, p->seq); failures++; break; }
        }
        if (count != n) { printf("trial %d: length %d != %d\n", trial, count, n); failures++; }

        // 2. each display list forms exactly one contiguous run,
        // 3. groups in first-appearance order, 4. stable within a group
        const void *groupOrder[16]; int groups = 0;
        const void *prev = NULL; int prevSeq = -1;
        bool closed[16]; memset(closed, 0, sizeof(closed));
        for (struct DisplayListNode *p = out; p != NULL; p = p->next) {
            if (p->displayList != prev) {
                // starting a new run: this dl must not have been closed already
                for (int g = 0; g < groups; g++) {
                    if (groupOrder[g] == p->displayList) {
                        printf("trial %d: display list run is not contiguous\n", trial);
                        failures++;
                    }
                }
                if (groups < 16) { groupOrder[groups++] = p->displayList; }
                prev = p->displayList;
                prevSeq = p->seq;
            } else {
                if (p->seq < prevSeq) {
                    printf("trial %d: unstable within group (%d after %d)\n", trial, p->seq, prevSeq);
                    failures++;
                }
                prevSeq = p->seq;
            }
        }
        // first-appearance order of groups
        int expect = 0;
        for (int i = 0; i < n && expect < groups; i++) {
            bool already = false;
            for (int g = 0; g < expect; g++) if (groupOrder[g] == nodes[i].displayList) already = true;
            if (already) continue;
            if (groupOrder[expect] != nodes[i].displayList) {
                printf("trial %d: group order wrong at %d\n", trial, expect);
                failures++;
                break;
            }
            expect++;
        }

        free(seen); free(nodes);
        (void) closed;
    }

    printf(failures ? "FAILURES: %d\n" : "all 20000 randomised trials passed\n", failures);
    return failures != 0;
}
