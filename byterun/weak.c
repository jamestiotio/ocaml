/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*              Damien Doligez, projet Para, INRIA Rocquencourt           */
/*                                                                        */
/*   Copyright 1997 Institut National de Recherche en Informatique et     */
/*     en Automatique.                                                    */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

#define CAML_INTERNALS

#define DEBUG_SPECIAL

/* Operations on weak arrays and ephemerons (named ephe here)*/

#include <string.h>

#include "caml/alloc.h"
#include "caml/fail.h"
#include "caml/major_gc.h"
#include "caml/memory.h"
#include "caml/mlvalues.h"
#include "caml/shared_heap.h"
#include "caml/weak.h"

value caml_dummy[] =
  {(value)Make_header(0,Abstract_tag, NOT_MARKABLE),
   Val_unit};
value caml_ephe_none = (value)&caml_dummy[1];

#define None_val (Val_int(0))
#define Some_tag 0

/** The minor heap is considered alive.
    Outside minor and major heap, x must be black.
*/
static inline int Is_Dead_during_clean(value x)
{
  CAMLassert (x != caml_ephe_none);
  CAMLassert (caml_gc_phase == Phase_sweep_ephe);
  return Is_block (x) && !Is_minor (x) && is_unmarked(x);
}

static inline int Must_be_Marked_during_mark (value x)
{
  CAMLassert (x != caml_ephe_none);
  CAMLassert (caml_gc_phase != Phase_sweep_ephe);
  return Is_block (x) && !Is_minor(x);
}

/* [len] is a value that represents a number of words (fields) */
CAMLprim value caml_ephe_create (value len)
{
  mlsize_t size, i;
  value res;
  caml_domain_state* domain_state = Caml_state;

  size = Long_val (len)
       + 1 /* weak_list */
       + 1 /* owning domain */
       + 1 /* the value */;
  if (size < CAML_EPHE_FIRST_KEY || size > Max_wosize) caml_invalid_argument ("Weak.create");
  res = caml_alloc_shr (size, Abstract_tag);

  Ephe_link(res) = domain_state->ephe_list_live;
  domain_state->ephe_list_live = res;
  Ephe_domain(res) = caml_domain_self();
  for (i = CAML_EPHE_DATA_OFFSET; i < size; i++)
    Op_val(res)[i] = caml_ephe_none;
  return res;
}

CAMLprim value caml_weak_create (value len)
{
#ifdef DEBUG_SPECIAL
  mlsize_t size, i;
  value res;

  size = Long_val (len) + 1 /* weak_list */ + 1 /* the value */;
  if (size <= 0 || size > Max_wosize) caml_invalid_argument ("Weak.create");
  res = caml_alloc_shr (size, 0);
  for (i = 1; i < size; i++)
    caml_initialize_field(res, i, None_val);
  return res;
#else
  return caml_ephe_create(len);
#endif
}

/**
   Specificity of the cleaning phase (Phase_clean):

   The dead keys must be removed from the ephemerons and data removed
   when one the keys is dead. Here we call it cleaning the ephemerons.
   A specific phase of the GC is dedicated to this, Phase_clean. This
   phase is just after the mark phase, so the white values are dead
   values. It iterates the function caml_ephe_clean through all the
   ephemerons.

   However the GC is incremental and ocaml code can run on the middle
   of this cleaning phase. In order to respect the semantic of the
   ephemerons concerning dead values, the getter and setter must work
   as if the cleaning of all the ephemerons have been done at once.

   - key getter: Even if a dead key have not yet been replaced by
     caml_ephe_none, getting it should return none.
   - key setter: If we replace a dead key we need to set the data to
     caml_ephe_none and clean the ephemeron.

     This two cases are dealt by a call to do_check_key_clean that
     trigger the cleaning of the ephemerons when the accessed key is
     dead. This test is fast.

     In the case of value getter and value setter, there is no fast
     test because the removing of the data depend of the deadliness of the keys.
     We must always try to clean the ephemerons.

 */

/* If we are in Phase_sweep_ephe we need to check if the key
   that is going to disappear is dead and so should trigger a cleaning
 */
static void do_check_key_clean(value e, mlsize_t offset)
{
  CAMLassert (offset >= CAML_EPHE_FIRST_KEY);
  CAMLassert (Ephe_domain(e) == caml_domain_self());

  if (caml_gc_phase == Phase_sweep_ephe) {
    value elt = Op_val(e)[offset];
    if (elt != caml_ephe_none && Is_Dead_during_clean(elt)) {
      Op_val(e)[offset] = caml_ephe_none;
      Op_val(e)[CAML_EPHE_DATA_OFFSET] = caml_ephe_none;
    }
  }
}
/* If we are in Phase_sweep_ephe we need to do as if the key is empty when
   it will be cleaned during this phase */
static int is_ephe_key_none (value e, mlsize_t offset)
{
  do_check_key_clean (e, offset);
  if (Op_val(e)[offset] == caml_ephe_none)
    return 1;
  return 0;
}

static void do_set (value e, mlsize_t offset, value v)
{
  CAMLassert (!(Is_block(v) && Is_foreign(v)));
  CAMLassert (Ephe_domain(e) == caml_domain_self());
  caml_domain_state* domain_state = Caml_state;

  if (Is_block(v) && Is_young(v)) {
    value old = Op_val(e)[offset];
    Op_val(e)[offset] = v;
    if (!(Is_block(old) && Is_young(old)))
      add_to_ephe_ref_table (&domain_state->remembered_set->ephe_ref,
                             e, offset);
  } else {
    Op_val(e)[offset] = v;
  }
}

CAMLprim value caml_ephe_set_key (value e, value n, value el)
{
  CAMLparam3(e,n,el);

  mlsize_t offset = Long_val (n) + CAML_EPHE_FIRST_KEY;
  if (offset < CAML_EPHE_FIRST_KEY || offset >= Wosize_val (e)){
    caml_invalid_argument ("Weak.set");
  }

  if (Ephe_domain(e) == caml_domain_self()) {
    do_check_key_clean(e,offset);
    do_set (e, offset, el);
  } else {
    caml_failwith ("caml_ephe_set_key");
  }
  CAMLreturn(Val_unit);
}

CAMLprim value caml_ephe_unset_key (value e, value n)
{
  return caml_ephe_set_key (e, n, caml_ephe_none);
}

value caml_ephe_set_key_option (value e, value n, value el)
{
  if (el != None_val && Is_block (el)) {
    return caml_ephe_set_key (e, n, el);
  } else {
    return caml_ephe_unset_key (e, n);
  }
}

CAMLprim value caml_weak_set (value ar, value n, value el)
{
#ifdef DEBUG_SPECIAL
  CAMLparam3(ar,n,el);
  mlsize_t offset = Long_val (n) + 2;
  if (offset < 2 || offset >= Wosize_val (ar)){
    caml_invalid_argument ("Weak.set");
  }
  if (el != None_val && Is_block (el)){
    CAMLassert (Wosize_val (el) == 1);
    caml_modify_field (ar, offset, Field (el, 0));
  }else{
    caml_modify_field (ar, offset, None_val);
  }
  CAMLreturn(Val_unit);
#else
  return caml_ephe_set_key_option(ar,n,el);
#endif
}

CAMLprim value caml_ephe_set_data (value e, value el)
{
  CAMLparam2(e,el);
  if (Ephe_domain(e) == caml_domain_self()) {
    if (caml_gc_phase == Phase_sweep_ephe)
      caml_ephe_clean(e);
    do_set (e, CAML_EPHE_DATA_OFFSET, el);
  } else {
    caml_failwith ("caml_ephe_set_data");
  }
  CAMLreturn(Val_unit);
}

CAMLprim value caml_ephe_unset_data (value e)
{
  return caml_ephe_set_data(e, caml_ephe_none);
}

CAMLprim value caml_ephe_get_key (value e, value n)
{
  CAMLparam2(e, n);
  CAMLlocal2 (res, elt);
  mlsize_t offset = Long_val (n) + CAML_EPHE_FIRST_KEY;

  if (offset < CAML_EPHE_FIRST_KEY || offset >= Wosize_val (e)){
    caml_invalid_argument ("Weak.get_key");
  }
  if (Ephe_domain(e) == caml_domain_self()) {
    elt = Op_val(e)[offset];
    CAMLassert (!(Is_block(elt) && Is_foreign(elt)));
    if (is_ephe_key_none(e, offset)) {
      res = None_val;
    } else {
      elt = Op_val(e)[offset];
      if (caml_gc_phase != Phase_sweep_ephe &&
          Must_be_Marked_during_mark(elt)) {
        caml_darken (0, elt, 0);
      }
      res = caml_alloc_small (1, Some_tag);
      caml_initialize_field(res, 0, elt);
    }
  } else {
    caml_failwith ("caml_ephe_get_key");
  }
  CAMLreturn (res);
}

CAMLprim value caml_weak_get (value ar, value n)
{
#ifdef DEBUG_SPECIAL
  CAMLparam2(ar, n);
  mlsize_t offset = Long_val (n) + 2;
  CAMLlocal2 (res, elt);
  if (offset < 2 || offset >= Wosize_val (ar)){
    caml_invalid_argument ("Weak.get_key");
  }
  caml_read_field(ar, offset, &elt);
  if (elt == None_val){
    res = None_val;
  }else{
    res = caml_alloc_small (1, Some_tag);
    caml_initialize_field(res, 0, elt);
  }
#else
  CAMLparam2(ar, n);
  CAMLlocal1(res);
  res = caml_ephe_get_key(ar, n);
#endif
  CAMLreturn (res);
}

CAMLprim value caml_ephe_get_data (value e)
{
  CAMLparam1 (e);
  CAMLlocal2 (res, elt);
  mlsize_t offset = CAML_EPHE_DATA_OFFSET;

  if (Ephe_domain(e) == caml_domain_self()) {
    CAMLassert (!(Is_block(elt) && Is_foreign(elt)));
    if (caml_gc_phase == Phase_sweep_ephe)
      caml_ephe_clean(e);
    elt = Op_val(e)[offset];
    if (elt == caml_ephe_none) {
      res = None_val;
    } else {
      if (caml_gc_phase != Phase_sweep_ephe &&
          Must_be_Marked_during_mark(elt)) {
        caml_darken (0, elt, 0);
      }
      res = caml_alloc_small (1, Some_tag);
      caml_initialize_field(res, 0, elt);
    }
  } else {
    caml_failwith ("caml_ephe_get_data");
  }
  CAMLreturn (res);
}

CAMLprim value caml_ephe_get_key_copy (value e, value n)
{
  caml_failwith ("caml_ephe_get_key_copy");
}

CAMLprim value caml_weak_get_copy (value e, value n){
  return caml_ephe_get_key_copy(e,n);
}

CAMLprim value caml_ephe_get_data_copy (value e)
{
  caml_failwith("caml_ephe_get_data_copy: not implemented");
}

CAMLprim value caml_ephe_check_key (value e, value n)
{
  CAMLparam2(e,n);

  mlsize_t offset = Long_val (n) + CAML_EPHE_FIRST_KEY;
  if (offset < CAML_EPHE_FIRST_KEY || offset >= Wosize_val (e)){
    caml_invalid_argument ("Weak.check");
  }
  if (Ephe_domain(e) == caml_domain_self()) {
    CAMLreturn(Val_bool(!is_ephe_key_none(e, offset)));
  } else {
    caml_failwith ("caml_ephe_check_key");
  }
}

CAMLprim value caml_weak_check (value e, value n)
{
  return caml_ephe_check_key(e,n);
}

CAMLprim value caml_ephe_check_data (value e)
{
  CAMLparam1(e);
  CAMLlocal1(v);

  if (Ephe_domain(e) == caml_domain_self()) {
    if (caml_gc_phase == Phase_sweep_ephe)
      caml_ephe_clean(e);
    v = Op_val(e)[CAML_EPHE_DATA_OFFSET];
    CAMLreturn(Val_bool(v != caml_ephe_none));
  } else {
    caml_failwith ("caml_ephe_check_data");
  }
}

CAMLprim value caml_ephe_blit_key (value ars, value ofs,
                               value ard, value ofd, value len)
{
  caml_failwith ("caml_ephe_blit_key");
}

CAMLprim value caml_ephe_blit_data (value ars, value ard)
{
  caml_failwith ("caml_ephe_blit_data");
}

CAMLprim value caml_weak_blit (value ars, value ofs,
                      value ard, value ofd, value len)
{
  return caml_ephe_blit_key (ars, ofs, ard, ofd, len);
}

void caml_ephe_clean (value v) {
  value child;
  int release_data = 0;
  mlsize_t size, i;
  header_t hd;
  CAMLassert (caml_gc_phase == Phase_sweep_ephe);
  CAMLassert (Ephe_domain(v) = caml_domain_self());

  hd = Hd_val(v);
  size = Wosize_hd (hd);
  for (i = CAML_EPHE_FIRST_KEY; i < size; i++) {
    child = Op_val(v)[i];
  ephemeron_again:
    if (child != caml_ephe_none && Is_block(child)) {
      if (Tag_val (child) == Forward_tag) {
        value f = Forward_val (child);
        if (Is_block(f)) {
          if (Tag_val(f) == Forward_tag || Tag_val(f) == Lazy_tag ||
              Tag_val(f) == Double_tag) {
            /* Do not short-circuit the pointer */
          } else {
            Op_val(v)[i] = child = f;
            if (Is_block (f) && Is_young (f))
              add_to_ephe_ref_table(&Caml_state->remembered_set->ephe_ref, v, i);
            goto ephemeron_again;
          }
        }
      }
      if (!Is_young (child) && is_unmarked(child)) {
        release_data = 1;
        Op_val(v)[i] = caml_ephe_none;
      }
    }
  }

  child = Op_val(v)[CAML_EPHE_DATA_OFFSET];
  if (child != caml_ephe_none) {
    if (release_data) {
      Op_val(v)[CAML_EPHE_DATA_OFFSET] = caml_ephe_none;
    } else {
      CAMLassert (!Is_block(child) && !is_unmarked(child));
    }
  }
}
