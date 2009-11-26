/* Routines related to .debug_info.

   Copyright (C) 2009 Red Hat, Inc.
   This file is part of Red Hat elfutils.

   Red Hat elfutils is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 2 of the License.

   Red Hat elfutils is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with Red Hat elfutils; if not, write to the Free Software Foundation,
   Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301 USA.

   Red Hat elfutils is an included package of the Open Invention Network.
   An included package of the Open Invention Network is a package for which
   Open Invention Network licensees cross-license their patents.  No patent
   license is granted, either expressly or impliedly, by designation as an
   included package.  Should you wish to participate in the Open Invention
   Network licensing program, please visit www.openinventionnetwork.com
   <http://www.openinventionnetwork.com>.  */

// xxx drop as soon as not necessary
#define __STDC_FORMAT_MACROS
#define PRI_DIE "DIE 0x%" PRIx64
#include <inttypes.h>

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <cassert>
#include <sstream>
#include "../libdw/dwarf.h"

#include "messages.h"
#include "low.h"
#include "checks-low.hh"
#include "pri.hh"
#include "config.h"
#include "check_debug_loc_range.hh"

namespace
{
  bool
  check_category (enum message_category cat)
  {
    return message_accept (&warning_criteria, cat);
  }

  bool
  check_die_references (struct cu *cu,
			struct ref_record *die_refs)
  {
    bool retval = true;
    for (size_t i = 0; i < die_refs->size; ++i)
      {
	struct ref *ref = die_refs->refs + i;
	if (!addr_record_has_addr (&cu->die_addrs, ref->addr))
	  {
	    wr_error (&ref->who,
		      ": unresolved reference to " PRI_DIE ".\n", ref->addr);
	    retval = false;
	  }
      }
    return retval;
  }

  bool
  check_global_die_references (struct cu *cu_chain)
  {
    bool retval = true;
    for (struct cu *it = cu_chain; it != NULL; it = it->next)
      for (size_t i = 0; i < it->die_refs.size; ++i)
	{
	  struct ref *ref = it->die_refs.refs + i;
	  struct cu *ref_cu = NULL;
	  for (struct cu *jt = cu_chain; jt != NULL; jt = jt->next)
	    if (addr_record_has_addr (&jt->die_addrs, ref->addr))
	      {
		ref_cu = jt;
		break;
	      }

	  if (ref_cu == NULL)
	    {
	      wr_error (ref->who)
		<< "unresolved (non-CU-local) reference to "
		<< pri::hex (ref->addr) << '.' << std::endl;
	      retval = false;
	    }
	  else if (ref_cu == it)
	    /* This is technically not a problem, so long as the
	       reference is valid, which it is.  But warn about this
	       anyway, perhaps local reference could be formed on fewer
	       number of bytes.  */
	    wr_message (mc_impact_2 | mc_acc_suboptimal | mc_die_rel,
			&ref->who,
			": local reference to " PRI_DIE " formed as global.\n",
			ref->addr);
	}

    return retval;
  }

  std::vector <cu_head>
  read_info_headers (struct elf_file *file,
		     struct sec *sec,
		     struct relocation_data *reloc)
  {
    struct read_ctx ctx;
    read_ctx_init (&ctx, sec->data, file->other_byte_order);

    std::vector <cu_head> ret;
    while (!read_ctx_eof (&ctx))
      {
	const unsigned char *cu_begin = ctx.ptr;
	struct where where = WHERE (sec_info, NULL);
	where_reset_1 (&where, read_ctx_get_offset (&ctx));

	cu_head head;
	head.offset = where.addr1;
	head.where = where;

	/* Reading CU head is a bit tricky, because we don't know if
	   we have run into (superfluous but allowed) zero padding
	   between CUs.  */
	if (!read_ctx_need_data (&ctx, 4)
	    && check_zero_padding (&ctx, cat (mc_info, mc_header), &where))
	  break;

	/* CU length.  In DWARF 2, (uint32_t)-1 is simply a CU of that
	   length.  In DWARF 3+ that's an escape for 64bit length.
	   Unfortunately to read CU version, we have to get through
	   this field.  So we just assume that (uint32_t)-1 is an
	   escape in all cases.  */
	uint32_t size32;
	if (!read_ctx_read_4ubyte (&ctx, &size32))
	  {
	    wr_error (where) << "can't read CU length." << std::endl;
	    throw check_base::failed ();
	  }
	if (size32 == 0
	    && check_zero_padding (&ctx, cat (mc_info, mc_header), &where))
	  break;

	Dwarf_Off cu_size;
	if (!read_size_extra (&ctx, size32, &cu_size,
			      &head.offset_size, &where))
	  throw check_base::failed ();

	if (!read_ctx_need_data (&ctx, cu_size))
	  {
	    wr_error (where)
	      << "section doesn't have enough data to read CU of size "
	      << cu_size << '.' << std::endl;
	    throw check_base::failed ();
	  }

	/* CU size captures the size from the end of the length field
	   to the end of the CU.  */
	const unsigned char *cu_end = ctx.ptr + cu_size;

	/* Version.  */
	uint16_t version;
	if (!read_ctx_read_2ubyte (&ctx, &version))
	  {
	    wr_error (head.where) << "can't read version." << std::endl;
	    throw check_base::failed ();
	  }
	if (get_dwarf_version (version) == NULL)
	  {
	    wr_error (head.where) << "unsupported CU version "
				  << version << '.' << std::endl;
	    throw check_base::failed ();
	  }
	if (version == 2 && head.offset_size == 8) // xxx?
	  /* Keep going.  It's a standard violation, but we may still
	     be able to read the unit under consideration and do
	     high-level checks.  */
	  wr_error (head.where) << "invalid 64-bit unit in DWARF 2 format.\n";
	head.version = version;

	/* Abbrev table offset.  */
	uint64_t ctx_offset = read_ctx_get_offset (&ctx);
	if (!read_ctx_read_offset (&ctx, head.offset_size == 8,
				   &head.abbrev_offset))
	  {
	    wr_error (head.where)
	      << "can't read abbrev table offset." << std::endl;
	    throw check_base::failed ();
	  }

	struct relocation *rel
	  = relocation_next (reloc, ctx_offset, &head.where, skip_ok);
	if (rel != NULL)
	  {
	    relocate_one (file, reloc, rel, head.offset_size,
			  &head.abbrev_offset, &head.where, sec_abbrev, NULL);
	    rel->invalid = true; // mark as invalid so it's skipped
				 // next time we pass by this
	  }
	else if (file->ehdr.e_type == ET_REL)
	  wr_message (head.where, cat (mc_impact_2, mc_info, mc_reloc))
	    << pri::lacks_relocation ("abbrev table offset") << std::endl;

	/* Address size.  */
	if (!read_address_size (file, &ctx, &head.address_size, &head.where))
	  throw check_base::failed ();

	head.head_size = ctx.ptr - cu_begin; // Length of the headers itself.
	head.total_size = cu_end - cu_begin; // Length including headers field.
	head.size = head.total_size - head.head_size;

	if (!read_ctx_skip (&ctx, head.size))
	  {
	    wr_error (where) << pri::not_enough ("next CU") << std::endl;
	    throw check_base::failed ();
	  }

	ret.push_back (head);
      }

    return ret;
  }

  section_id
  reloc_target (uint8_t form, struct abbrev_attrib *at)
  {
    switch (form)
      {
      case DW_FORM_strp:
	return sec_str;

      case DW_FORM_addr:

	switch (at->name)
	  {
	  case DW_AT_low_pc:
	  case DW_AT_high_pc:
	  case DW_AT_entry_pc:
	    return rel_exec;

	  case DW_AT_const_value:
	    /* Appears in some kernel modules.  It's not allowed by the
	       standard, but leave that for high-level checks.  */
	    return rel_address;
	  };

	break;

      case DW_FORM_ref_addr:
	return sec_info;

      case DW_FORM_data1:
      case DW_FORM_data2:
	/* While these are technically legal, they are never used in
	   DWARF sections.  So better mark them as illegal, and have
	   dwarflint flag them.  */
	return sec_invalid;

      case DW_FORM_data4:
      case DW_FORM_data8:

	switch (at->name)
	  {
	  case DW_AT_stmt_list:
	    return sec_line;

	  case DW_AT_location:
	  case DW_AT_string_length:
	  case DW_AT_return_addr:
	  case DW_AT_data_member_location:
	  case DW_AT_frame_base:
	  case DW_AT_segment:
	  case DW_AT_static_link:
	  case DW_AT_use_location:
	  case DW_AT_vtable_elem_location:
	    return sec_loc;

	  case DW_AT_mac_info:
	    return sec_mac;

	  case DW_AT_ranges:
	    return sec_ranges;
	  }

	break;

      case DW_FORM_string:
      case DW_FORM_ref1:
      case DW_FORM_ref2:
      case DW_FORM_ref4:
	/* Shouldn't be relocated.  */
	return sec_invalid;

      case DW_FORM_sdata:
      case DW_FORM_udata:
      case DW_FORM_flag:
      case DW_FORM_flag_present:
      case DW_FORM_ref_udata:
	assert (!"Can't be relocated!");

      case DW_FORM_block1:
      case DW_FORM_block2:
      case DW_FORM_block4:
      case DW_FORM_block:
	assert (!"Should be handled specially!");
      };

    std::cout << "XXX don't know how to handle form=" << pri::form (form)
	      << ", at=" << pri::attr (at->name) << std::endl;

    return rel_value;
  }

  struct value_check_cb_ctx
  {
    struct read_ctx *const ctx;
    struct where *const where;
    struct cu *const cu;
    struct ref_record *local_die_refs;
    Elf_Data *strings;
    struct coverage *strings_coverage;
    struct cu_coverage *cu_coverage;
  };

  typedef void (*value_check_cb_t) (uint64_t addr,
				    struct value_check_cb_ctx const *ctx);

  /* Callback for local DIE references.  */
  void
  check_die_ref_local (uint64_t addr, struct value_check_cb_ctx const *ctx)
  {
    assert (ctx->ctx->end > ctx->ctx->begin);
    if (addr > (uint64_t)(ctx->ctx->end - ctx->ctx->begin))
      {
	wr_error (ctx->where,
		  ": invalid reference outside the CU: 0x%" PRIx64 ".\n",
		  addr);
	return;
      }

    if (ctx->local_die_refs != NULL)
      /* Address holds a CU-local reference, so add CU offset
	 to turn it into section offset.  */
      ref_record_add (ctx->local_die_refs,
		      addr + ctx->cu->head->offset, ctx->where);
  }

  /* Callback for global DIE references.  */
  void
  check_die_ref_global (uint64_t addr, struct value_check_cb_ctx const *ctx)
  {
    ref_record_add (&ctx->cu->die_refs, addr, ctx->where);
  }

  /* Callback for strp values.  */
  void
  check_strp (uint64_t addr, struct value_check_cb_ctx const *ctx)
  {
    if (ctx->strings == NULL)
      wr_error (ctx->where, ": strp attribute, but no .debug_str data.\n");
    else if (addr >= ctx->strings->d_size)
      wr_error (ctx->where,
		": Invalid offset outside .debug_str: 0x%" PRIx64 ".\n",
		addr);
    else
      {
	/* Record used part of .debug_str.  */
	const char *buf = static_cast <const char *> (ctx->strings->d_buf);
	const char *startp = buf + addr;
	const char *data_end = buf + ctx->strings->d_size;
	const char *strp = startp;
	while (strp < data_end && *strp != 0)
	  ++strp;
	if (strp == data_end)
	  wr_error (ctx->where,
		    ": String at .debug_str: 0x%" PRIx64
		    " is not zero-terminated.\n", addr);

	if (ctx->strings_coverage != NULL)
	  coverage_add (ctx->strings_coverage, addr, strp - startp + 1);
      }
  }

  /* Callback for rangeptr values.  */
  void
  check_rangeptr (uint64_t value, struct value_check_cb_ctx const *ctx)
  {
    if ((value % ctx->cu->head->address_size) != 0)
      wr_message (mc_ranges | mc_impact_2, ctx->where,
		  ": rangeptr value %#" PRIx64
		  " not aligned to CU address size.\n", value);
    ctx->cu_coverage->need_ranges = true;
    ref_record_add (&ctx->cu->range_refs, value, ctx->where);
  }

  /* Callback for lineptr values.  */
  void
  check_lineptr (uint64_t value, struct value_check_cb_ctx const *ctx)
  {
    ref_record_add (&ctx->cu->line_refs, value, ctx->where);
  }

  /* Callback for locptr values.  */
  void
  check_locptr (uint64_t value, struct value_check_cb_ctx const *ctx)
  {
    ref_record_add (&ctx->cu->loc_refs, value, ctx->where);
  }

  /*
    Returns:
    -1 in case of error
    +0 in case of no error, but the chain only consisted of a
       terminating zero die.
    +1 in case some dies were actually loaded
  */
  int
  read_die_chain (dwarf_version_h ver,
		  struct elf_file *file,
		  struct read_ctx *ctx,
		  struct cu *cu,
		  struct abbrev_table *abbrevs,
		  Elf_Data *strings,
		  struct ref_record *local_die_refs,
		  struct coverage *strings_coverage,
		  struct relocation_data *reloc,
		  struct cu_coverage *cu_coverage)
  {
    bool got_die = false;
    uint64_t sibling_addr = 0;
    uint64_t die_off, prev_die_off = 0;
    struct abbrev *abbrev = NULL;
    struct abbrev *prev_abbrev = NULL;
    struct where where = WHERE (sec_info, NULL);

    struct value_check_cb_ctx cb_ctx = {
      ctx, &where, cu,
      local_die_refs,
      strings, strings_coverage,
      cu_coverage
    };

    while (!read_ctx_eof (ctx))
      {
	where = cu->head->where;
	die_off = read_ctx_get_offset (ctx);
	/* Shift reported DIE offset by CU offset, to match the way
	   readelf reports DIEs.  */
	where_reset_2 (&where, die_off + cu->head->offset);

	uint64_t abbr_code;

	if (!checked_read_uleb128 (ctx, &abbr_code, &where, "abbrev code"))
	  return -1;

#define DEF_PREV_WHERE							\
	struct where prev_where = where;				\
	where_reset_2 (&prev_where, prev_die_off + cu->head->offset)

	/* Check sibling value advertised last time through the loop.  */
	if (sibling_addr != 0)
	  {
	    if (abbr_code == 0)
	      wr_error (&where,
			": is the last sibling in chain, "
			"but has a DW_AT_sibling attribute.\n");
	    else if (sibling_addr != die_off)
	      {
		DEF_PREV_WHERE;
		wr_error (&prev_where,
			  ": This DIE claims that its sibling is 0x%"
			  PRIx64 ", but it's actually 0x%" PRIx64 ".\n",
			  sibling_addr, die_off);
	      }
	    sibling_addr = 0;
	  }
	else if (abbr_code != 0
		 && abbrev != NULL && abbrev->has_children)
	  {
	    /* Even if it has children, the DIE can't have a sibling
	       attribute if it's the last DIE in chain.  That's the
	       reason we can't simply check this when loading
	       abbrevs.  */
	    DEF_PREV_WHERE;
	    wr_message (prev_where, cat (mc_die_rel, mc_acc_suboptimal,
					 mc_impact_4))
	      << "This DIE had children, but no DW_AT_sibling attribute."
	      << std::endl;
	  }
#undef DEF_PREV_WHERE

	prev_die_off = die_off;

	/* The section ended.  */
	if (abbr_code == 0)
	  break;
	if (read_ctx_eof (ctx))
	  {
	    wr_error (where)
	      << "DIE chain not terminated with DIE with zero abbrev code."
	      << std::endl;
	    break;
	  }

	prev_die_off = die_off;
	got_die = true;
	if (dump_die_offsets)
	  fprintf (stderr, "%s: abbrev %" PRId64 "\n",
		   where_fmt (&where, NULL), abbr_code);

	/* Find the abbrev matching the code.  */
	prev_abbrev = abbrev;
	abbrev = abbrev_table_find_abbrev (abbrevs, abbr_code);
	if (abbrev == NULL)
	  {
	    wr_error (&where,
		      ": abbrev section at 0x%" PRIx64
		      " doesn't contain code %" PRIu64 ".\n",
		      abbrevs->offset, abbr_code);
	    return -1;
	  }
	abbrev->used = true;

	addr_record_add (&cu->die_addrs, cu->head->offset + die_off);

	uint64_t low_pc = (uint64_t)-1, high_pc = (uint64_t)-1;
	bool low_pc_relocated = false, high_pc_relocated = false;
	GElf_Sym low_pc_symbol_mem, *low_pc_symbol = &low_pc_symbol_mem;
	GElf_Sym high_pc_symbol_mem, *high_pc_symbol = &high_pc_symbol_mem;

	/* Attribute values.  */
	for (struct abbrev_attrib *it = abbrev->attribs;
	     it->name != 0; ++it)
	  {
	    where.ref = &it->where;

	    uint8_t form = it->form;
	    bool indirect = form == DW_FORM_indirect;
	    if (indirect)
	      {
		uint64_t value;
		if (!checked_read_uleb128 (ctx, &value, &where,
					   "indirect attribute form"))
		  return -1;

		if (!dwver_form_valid (ver, form))
		  {
		    wr_error (where)
		      << "invalid indirect form " << pri::hex (value)
		      << '.' << std::endl;
		    return -1;
		  }
		form = value;

		if (it->name == DW_AT_sibling)
		  switch (check_sibling_form (ver, form))
		    {
		    case -1:
		      wr_message (where, cat (mc_die_rel, mc_impact_2))
			<< "DW_AT_sibling attribute with (indirect) form "
			"DW_FORM_ref_addr." << std::endl;
		      break;

		    case -2:
		      wr_error (where)
			<< "DW_AT_sibling attribute with non-reference "
			"(indirect) form \"" << pri::form (value)
			<< "\"." << std::endl;
		    };
	      }

	    value_check_cb_t value_check_cb = NULL;

	    /* For checking lineptr, rangeptr, locptr.  */
	    bool check_someptr = false;
	    enum message_category extra_mc = mc_none;

	    uint64_t ctx_offset = read_ctx_get_offset (ctx) + cu->head->offset;
	    bool type_is_rel = file->ehdr.e_type == ET_REL;

	    /* Attribute value.  */
	    uint64_t value = 0;

	    /* Whether the value should be relocated first.  Note that
	       relocations are really required only in REL files, so
	       missing relocations are not warned on even with
	       rel_require, unless type_is_rel.  */
	    enum
	    {
	      rel_no,		// don't allow a relocation
	      rel_require,	// require a relocation
	      rel_nonzero,	// require a relocation if value != 0
	    } relocate = rel_no;
	    size_t width = 0;

	    /* Point to variable that you want to copy relocated value
	       to.  */
	    uint64_t *valuep = NULL;

	    /* Point to variable that you want set to `true' in case the
	       value was relocated.  */
	    bool *relocatedp = NULL;

	    /* Point to variable that you want set to symbol that the
	       relocation was made against.  */
	    GElf_Sym **symbolp = NULL;

	    /* Setup locptr checking.  */
	    if (is_location_attrib (it->name))
	      {
		switch (form)
		  {
		  case DW_FORM_data8:
		    if (cu->head->offset_size == 4)
		      wr_error (where)
			<< "location attribute with form \""
			<< pri::form (form) << "\" in 32-bit CU." << std::endl;
		    /* fall-through */

		  case DW_FORM_data4:
		  case DW_FORM_sec_offset:
		    value_check_cb = check_locptr;
		    extra_mc = mc_loc;
		    check_someptr = true;
		    break;

		  case DW_FORM_block1:
		  case DW_FORM_block2:
		  case DW_FORM_block4:
		  case DW_FORM_block:
		    break;

		  default:
		    /* Only print error if it's indirect.  Otherwise we
		       gave diagnostic during abbrev loading.  */
		    if (indirect)
		      wr_error (where)
			<< "location attribute with invalid (indirect) form \""
			<< pri::form (form) << "\"." << std::endl;
		  };
	      }
	    /* Setup rangeptr or lineptr checking.  */
	    else if (it->name == DW_AT_ranges
		     || it->name == DW_AT_stmt_list)
	      switch (form)
		{
		case DW_FORM_data8:
		  if (cu->head->offset_size == 4)
		    // xxx could now also be checked during abbrev loading
		    wr_error (where)
		      << pri::attr (it->name)
		      << " with form DW_FORM_data8 in 32-bit CU." << std::endl;
		  /* fall-through */

		case DW_FORM_data4:
		case DW_FORM_sec_offset:
		  check_someptr = true;
		  if (it->name == DW_AT_ranges)
		    {
		      value_check_cb = check_rangeptr;
		      extra_mc = mc_ranges;
		    }
		  else
		    {
		      assert (it->name == DW_AT_stmt_list);
		      value_check_cb = check_lineptr;
		      extra_mc = mc_line;
		    }
		  break;

		default:
		  /* Only print error if it's indirect.  Otherwise we
		     gave diagnostic during abbrev loading.  */
		  if (indirect)
		    wr_error (where)
		      << pri::attr (it->name)
		      << " with invalid (indirect) form \"" << pri::form (form)
		      << "\"." << std::endl;
		}
	    /* Setup low_pc and high_pc checking.  */
	    else if (it->name == DW_AT_low_pc)
	      {
		relocatedp = &low_pc_relocated;
		symbolp = &low_pc_symbol;
		valuep = &low_pc;
	      }
	    else if (it->name == DW_AT_high_pc)
	      {
		relocatedp = &high_pc_relocated;
		symbolp = &high_pc_symbol;
		valuep = &high_pc;
	      }

	    /* Load attribute value and setup per-form checking.  */
	    switch (form)
	      {
	      case DW_FORM_strp:
		value_check_cb = check_strp;
	      case DW_FORM_sec_offset:
		if (!read_ctx_read_offset (ctx, cu->head->offset_size == 8,
					   &value))
		  {
		  cant_read:
		    wr_error (where)
		      << "can't read value of attribute "
		      << pri::attr (it->name) << '.' << std::endl;
		    return -1;
		  }

		relocate = rel_require;
		width = cu->head->offset_size;
		break;

	      case DW_FORM_string:
		if (!read_ctx_read_str (ctx))
		  goto cant_read;
		break;

	      case DW_FORM_ref_addr:
		value_check_cb = check_die_ref_global;
		width = cu->head->offset_size;

		if (cu->head->version == 2)
		case DW_FORM_addr:
		  width = cu->head->address_size;

		if (!read_ctx_read_offset (ctx, width == 8, &value))
		  goto cant_read;

		/* In non-rel files, neither addr, nor ref_addr /need/
		   a relocation.  */
		relocate = rel_nonzero;
		break;

	      case DW_FORM_ref_udata:
		value_check_cb = check_die_ref_local;
	      case DW_FORM_udata:
		if (!checked_read_uleb128 (ctx, &value, &where,
					   "attribute value"))
		  return -1;
		break;

	      case DW_FORM_flag_present:
		value = 1;
		break;

	      case DW_FORM_ref1:
		value_check_cb = check_die_ref_local;
	      case DW_FORM_flag:
	      case DW_FORM_data1:
		if (!read_ctx_read_var (ctx, 1, &value))
		  goto cant_read;
		break;

	      case DW_FORM_ref2:
		value_check_cb = check_die_ref_local;
	      case DW_FORM_data2:
		if (!read_ctx_read_var (ctx, 2, &value))
		  goto cant_read;
		break;

	      case DW_FORM_data4:
		if (check_someptr)
		  {
		    relocate = rel_require;
		    width = 4;
		  }
		if (false)
	      case DW_FORM_ref4:
		  value_check_cb = check_die_ref_local;
		if (!read_ctx_read_var (ctx, 4, &value))
		  goto cant_read;
		break;

	      case DW_FORM_data8:
		if (check_someptr)
		  {
		    relocate = rel_require;
		    width = 8;
		  }
		if (false)
		case DW_FORM_ref8:
		  value_check_cb = check_die_ref_local;
		if (!read_ctx_read_8ubyte (ctx, &value))
		  goto cant_read;
		break;

	      case DW_FORM_sdata:
		{
		  int64_t value64;
		  if (!checked_read_sleb128 (ctx, &value64, &where,
					     "attribute value"))
		    return -1;
		  value = (uint64_t) value64;
		  break;
		}

	      case DW_FORM_block:
		{
		  uint64_t length;

		  if (false)
		  case DW_FORM_block1:
		    width = 1;

		  if (false)
		  case DW_FORM_block2:
		    width = 2;

		  if (false)
		  case DW_FORM_block4:
		    width = 4;

		  if (width == 0)
		    {
		      if (!checked_read_uleb128 (ctx, &length, &where,
						 "attribute value"))
			return -1;
		    }
		  else if (!read_ctx_read_var (ctx, width, &length))
		    goto cant_read;

		  if (is_location_attrib (it->name))
		    {
		      uint64_t expr_start
			= cu->head->offset + read_ctx_get_offset (ctx);
		      if (!check_location_expression (file, ctx, cu, expr_start,
						      reloc, length, &where))
			return -1;
		    }
		  else
		    /* xxx really skip_mismatched?  We just don't know
		       how to process these...  */
		    relocation_skip (reloc,
				     read_ctx_get_offset (ctx) + length,
				     &where, skip_mismatched);

		  if (!read_ctx_skip (ctx, length))
		    goto cant_read;
		  break;
		}

	      case DW_FORM_indirect:
		wr_error (&where, ": indirect form is again indirect.\n");
		return -1;

	      default:
		wr_error (&where,
			  ": internal error: unhandled form 0x%x.\n", form);
	      }

	    /* Relocate the value if appropriate.  */
	    struct relocation *rel;
	    if ((rel = relocation_next (reloc, ctx_offset,
					&where, skip_mismatched)))
	      {
		if (relocate == rel_no)
		  wr_message (where, cat (mc_impact_4, mc_die_other,
					  mc_reloc, extra_mc))
		    << "unexpected relocation of " << pri::form (form) << '.'
		    << std::endl;

		relocate_one (file, reloc, rel, width, &value, &where,
			      reloc_target (form, it), symbolp);

		if (relocatedp != NULL)
		  *relocatedp = true;
	      }
	    else
	      {
		if (symbolp != NULL)
		  WIPE (*symbolp);
		if (type_is_rel
		    && (relocate == rel_require
			|| (relocate == rel_nonzero
			    && value != 0)))
		  wr_message (where, cat (mc_impact_2, mc_die_other,
					  mc_reloc, extra_mc))
		    << pri::lacks_relocation (pri::form (form)) << std::endl;
	      }

	    /* Dispatch value checking.  */
	    if (it->name == DW_AT_sibling)
	      {
		/* Full-blown DIE reference checking is too heavy-weight
		   and not practical (error messages wise) for checking
		   siblings.  */
		assert (value_check_cb == check_die_ref_local
			|| value_check_cb == check_die_ref_global);
		valuep = &sibling_addr;
	      }
	    else if (value_check_cb != NULL)
	      value_check_cb (value, &cb_ctx);

	    /* Store the relocated value.  Note valuep may point to
	       low_pc or high_pc.  */
	    if (valuep != NULL)
	      *valuep = value;

	    /* Check PC coverage.  */
	    if (abbrev->tag == DW_TAG_compile_unit
		|| abbrev->tag == DW_TAG_partial_unit)
	      {
		if (it->name == DW_AT_low_pc)
		  cu->low_pc = value;

		if (low_pc != (uint64_t)-1 && high_pc != (uint64_t)-1)
		  coverage_add (&cu_coverage->cov, low_pc, high_pc - low_pc);
	      }
	  }
	where.ref = NULL;

	if (high_pc != (uint64_t)-1 && low_pc != (uint64_t)-1)
	  {
	    if (high_pc_relocated != low_pc_relocated)
	      wr_message (where, cat (mc_die_other, mc_impact_2, mc_reloc))
		<< "only one of DW_AT_low_pc and DW_AT_high_pc is relocated."
		<< std::endl;
	    else
	      check_range_relocations (mc_die_other, &where,
				       file,
				       low_pc_symbol, high_pc_symbol,
				       "DW_AT_low_pc and DW_AT_high_pc");
	  }

	where.ref = &abbrev->where;

	if (abbrev->has_children)
	  {
	    int st = read_die_chain (ver, file, ctx, cu, abbrevs, strings,
				     local_die_refs,
				     strings_coverage, reloc,
				     cu_coverage);
	    if (st == -1)
	      return -1;
	    else if (st == 0)
	      wr_message (mc_impact_3 | mc_acc_suboptimal | mc_die_rel,
			  &where,
			  ": abbrev has_children, but the chain was empty.\n");
	  }
      }

    if (sibling_addr != 0)
      wr_error (&where,
		": this DIE should have had its sibling at 0x%"
		PRIx64 ", but the DIE chain ended.\n", sibling_addr);

    return got_die ? 1 : 0;
  }
  bool
  check_cu_structural (struct elf_file *file,
		       struct read_ctx *ctx,
		       struct cu *const cu,
		       struct abbrev_table *abbrev_chain,
		       Elf_Data *strings,
		       struct coverage *strings_coverage,
		       struct relocation_data *reloc,
		       struct cu_coverage *cu_coverage)
  {
    if (dump_die_offsets)
      fprintf (stderr, "%s: CU starts\n", where_fmt (&cu->head->where, NULL));
    bool retval = true;

    dwarf_version_h ver = get_dwarf_version (cu->head->version);
    assert (ver != NULL);

    /* Look up Abbrev table for this CU.  */
    struct abbrev_table *abbrevs = abbrev_chain;
    for (; abbrevs != NULL; abbrevs = abbrevs->next)
      if (abbrevs->offset == cu->head->abbrev_offset)
	break;

    if (abbrevs == NULL)
      {
	wr_error (&cu->head->where,
		  ": couldn't find abbrev section with offset %" PRId64 ".\n",
		  cu->head->abbrev_offset);
	return false;
      }

    /* Read DIEs.  */
    struct ref_record local_die_refs;
    WIPE (local_die_refs);

    cu->cudie_offset = read_ctx_get_offset (ctx) + cu->head->offset;
    if (read_die_chain (ver, file, ctx, cu, abbrevs, strings,
			&local_die_refs, strings_coverage,
			(reloc != NULL && reloc->size > 0) ? reloc : NULL,
			cu_coverage) < 0)
      {
	abbrevs->skip_check = true;
	retval = false;
      }
    else if (!check_die_references (cu, &local_die_refs))
      retval = false;

    ref_record_free (&local_die_refs);
    return retval;
  }

  struct cu *
  check_info_structural (struct elf_file *file,
			 struct sec *sec,
			 struct abbrev_table *abbrev_chain,
			 Elf_Data *strings,
			 struct cu_coverage *cu_coverage,
			 std::vector <cu_head> const &cu_headers)
  {
    struct ref_record die_refs;
    WIPE (die_refs);

    struct cu *cu_chain = NULL;
    bool success = true;

    struct coverage strings_coverage_mem, *strings_coverage = NULL;
    if (strings != NULL && check_category (mc_strings))
      {
	WIPE (strings_coverage_mem);
	strings_coverage = &strings_coverage_mem;
      }

    struct relocation_data *reloc = sec->rel.size > 0 ? &sec->rel : NULL;
    if (reloc != NULL)
      relocation_reset (reloc);

    struct read_ctx ctx;
    read_ctx_init (&ctx, sec->data, file->other_byte_order);
    for (std::vector <cu_head>::const_iterator it = cu_headers.begin ();
	 it != cu_headers.end (); ++it)
      {
	cu_head const &head = *it;
	where const &where = head.where;
	struct cu *cur = (cu *)xcalloc (1, sizeof (*cur));
	cur->head = &head;
	cur->low_pc = (uint64_t)-1;
	cur->next = cu_chain;
	cu_chain = cur;

	assert (read_ctx_need_data (&ctx, head.total_size));

	// Make CU context begin just before the CU length, so that
	// DIE offsets are computed correctly.
	struct read_ctx cu_ctx;
	const unsigned char *cu_end = ctx.ptr + head.total_size;
	read_ctx_init_sub (&cu_ctx, &ctx, ctx.ptr, cu_end);
	cu_ctx.ptr += head.head_size;

	if (!check_cu_structural (file, &cu_ctx, cur, abbrev_chain,
				  strings, strings_coverage, reloc,
				  cu_coverage))
	  {
	    success = false;
	    break;
	  }

	if (cu_ctx.ptr != cu_ctx.end
	    && !check_zero_padding (&cu_ctx, mc_info, &where))
	  {
	    // Garbage coordinates:
	    uint64_t start
	      = read_ctx_get_offset (&ctx) + read_ctx_get_offset (&cu_ctx);
	    uint64_t end = read_ctx_get_offset (&ctx) + head.total_size;
	    wr_message_padding_n0 (mc_info, &where, start, end);
	  }

	int i = read_ctx_skip (&ctx, head.total_size);
	assert (i);
      }

    if (success)
      {
	if (ctx.ptr != ctx.end)
	  /* Did we read up everything?  */
	  {
	    where wh = WHERE (sec_info, NULL);
	    wr_message (cat (mc_die_other, mc_impact_4), &wh,
			": CU lengths don't exactly match Elf_Data contents.");
	  }
	else
	  /* Did we consume all the relocations?  */
	  relocation_skip_rest (&sec->rel, sec->id);

	/* If we managed to read up everything, now do abbrev usage
	   analysis.  */
	for (struct abbrev_table *abbrevs = abbrev_chain;
	     abbrevs != NULL; abbrevs = abbrevs->next)
	  if (abbrevs->used && !abbrevs->skip_check)
	    for (size_t i = 0; i < abbrevs->size; ++i)
	      if (!abbrevs->abbr[i].used)
		wr_message (mc_impact_3 | mc_acc_bloat | mc_abbrevs,
			    &abbrevs->abbr[i].where,
			    ": abbreviation is never used.\n");
      }


    /* We used to check that all CUs have the same address size.  Now
       that we validate address_size of each CU against the ELF header,
       that's not necessary anymore.  */

    bool references_sound = check_global_die_references (cu_chain);
    ref_record_free (&die_refs);

    if (strings_coverage != NULL)
      {
	if (success)
	  {
	    struct hole_info info = {sec_str, mc_strings, strings->d_buf, 0};
	    coverage_find_holes (strings_coverage, 0, strings->d_size,
				 found_hole, &info);
	  }
	coverage_free (strings_coverage);
      }

    if (!success || !references_sound)
      {
	cu_free (cu_chain);
	cu_chain = NULL;
      }

    /* Reverse the chain, so that it's organized "naturally".  Has
       significant impact on performance when handling loc_ref and
       range_ref fields in loc/range validation.  */
    struct cu *last = NULL;
    for (struct cu *it = cu_chain; it != NULL; )
      {
	struct cu *next = it->next;
	it->next = last;
	last = it;
	it = next;
      }
    cu_chain = last;

    return cu_chain;
  }
}

read_cu_headers::read_cu_headers (dwarflint &lint)
  : _m_sec_info (lint.check (_m_sec_info))
  , cu_headers (read_info_headers (&_m_sec_info->file,
				   &_m_sec_info->sect,
				   _m_sec_info->reldata ()))
{
}

check_debug_info::check_debug_info (dwarflint &lint)
  : _m_sec_info (lint.check (_m_sec_info))
  , _m_sec_abbrev (lint.check (_m_sec_abbrev))
  , _m_sec_str (lint.check (_m_sec_str))
  , _m_abbrevs (lint.check (_m_abbrevs))
  , _m_cu_headers (lint.check (_m_cu_headers))
{
  memset (&cu_cov, 0, sizeof (cu_cov));

  cu *chain = check_info_structural
    (&_m_sec_info->file, &_m_sec_info->sect,
     &_m_abbrevs->abbrevs.begin ()->second,
     _m_sec_str->sect.data, &cu_cov,
     _m_cu_headers->cu_headers);

  if (chain == NULL)
    throw check_base::failed ();

  for (cu *cu = chain; cu != NULL; cu = cu->next)
    cus.push_back (*cu);

  // re-link CUs so that they form a chain again.  This is to
  // interface with C-level code.  The last CU's next is null, so we
  // don't have to re-link it.
  cu *last = NULL;
  for (std::vector<cu>::iterator it = cus.begin ();
       it != cus.end (); ++it)
    {
      cu *cur = &*it;
      if (last != NULL)
	last->next = cur;
      last = cur;
    }

  if (cus.size () > 0)
    assert (cus.back ().next == NULL);
}

check_debug_info::~check_debug_info ()
{
  cu_free (&cus.back ());
}
