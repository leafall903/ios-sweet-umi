/* Two cache-vtable slots: a failed patch restores both values and RO pages.
 * Pure callbacks permit host failure injection; no guessed native addresses. */
#ifndef UMI165_SLOT_TRANSACTION_H
#define UMI165_SLOT_TRANSACTION_H
#include <stdint.h>
struct umi165_slot_ops {
 void *ctx;
 int (*read)(void *,unsigned,uintptr_t *);
 int (*protect)(void *,unsigned,int);
 int (*write)(void *,unsigned,uintptr_t);
};
struct umi165_slot_transaction {
 struct umi165_slot_ops ops;
 uintptr_t original[2],replacement[2];
 unsigned touched;
};
/* -1 means state/protection cannot be proved restored: caller must exit. */
static int umi165_slots_restore(struct umi165_slot_transaction *t) {
 int ok=1;
 for(unsigned n=2;n>0;n--) {
  unsigned i=n-1;if(!(t->touched&(1u<<i)))continue;
  uintptr_t value=0;
  if(!t->ops.read(t->ops.ctx,i,&value)) {ok=0;continue;}
  if(value!=t->original[i]) {
   if(value!=t->replacement[i] || !t->ops.protect(t->ops.ctx,i,1)) {ok=0;continue;}
   int wrote=t->ops.write(t->ops.ctx,i,t->original[i]);
   int read=t->ops.read(t->ops.ctx,i,&value);
   if(!wrote || !read || value!=t->original[i])ok=0;
  }
  if(!t->ops.protect(t->ops.ctx,i,0))ok=0;
 }
 if(ok)t->touched=0;
 return ok?0:-1;
}
/* 1 installed; 0 rejected with originals restored; -1 requires caller exit. */
static int umi165_slots_install(struct umi165_slot_transaction *t) {
 uintptr_t value=0;t->touched=0;
 for(unsigned i=0;i<2;i++)
  if(!t->ops.read(t->ops.ctx,i,&value) || value!=t->original[i])return 0;
 for(unsigned i=0;i<2;i++) {
  if(!t->ops.read(t->ops.ctx,i,&value) || value!=t->original[i])goto fail;
  /* Mark before protection/write, covering an operation that changed state
   * before reporting failure. */
  t->touched|=1u<<i;
  if(!t->ops.protect(t->ops.ctx,i,1))goto fail;
  if(!t->ops.write(t->ops.ctx,i,t->replacement[i]))goto fail;
  if(!t->ops.read(t->ops.ctx,i,&value) || value!=t->replacement[i])goto fail;
  if(!t->ops.protect(t->ops.ctx,i,0))goto fail;
 }
 return 1;
fail:
 return umi165_slots_restore(t);
}
#endif
