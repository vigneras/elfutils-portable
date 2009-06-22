/* elfutils::dwarf_edit attribute value interfaces.
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

   In addition, as a special exception, Red Hat, Inc. gives You the
   additional right to link the code of Red Hat elfutils with code licensed
   under any Open Source Initiative certified open source license
   (http://www.opensource.org/licenses/index.php) which requires the
   distribution of source code with any binary distribution and to
   distribute linked combinations of the two.  Non-GPL Code permitted under
   this exception must only link to the code of Red Hat elfutils through
   those well defined interfaces identified in the file named EXCEPTION
   found in the source code files (the "Approved Interfaces").  The files
   of Non-GPL Code may instantiate templates or use macros or inline
   functions from the Approved Interfaces without causing the resulting
   work to be covered by the GNU General Public License.  Only Red Hat,
   Inc. may make changes or additions to the list of Approved Interfaces.
   Red Hat's grant of this exception is conditioned upon your not adding
   any new exceptions.  If you wish to add a new Approved Interface or
   exception, please contact Red Hat.  You must obey the GNU General Public
   License in all respects for all of the Red Hat elfutils code and other
   code used in conjunction with Red Hat elfutils except the Non-GPL Code
   covered by this exception.  If you modify this file, you may extend this
   exception to your version of the file, but you are not obligated to do
   so.  If you do not wish to provide this exception without modification,
   you must delete this exception statement from your version and license
   this file solely under the GPL without exception.

   Red Hat elfutils is an included package of the Open Invention Network.
   An included package of the Open Invention Network is a package for which
   Open Invention Network licensees cross-license their patents.  No patent
   license is granted, either expressly or impliedly, by designation as an
   included package.  Should you wish to participate in the Open Invention
   Network licensing program, please visit www.openinventionnetwork.com
   <http://www.openinventionnetwork.com>.  */

#include <config.h>
#include "dwarf_edit"

#include <typeinfo>

using namespace elfutils;


dwarf::value_space
dwarf_edit::attr_value::what_space () const
{
  if (typeid (*_m_value) == typeid (value_flag))
    return dwarf::VS_flag;
  if (typeid (*_m_value) == typeid (value_dwarf_constant))
    return dwarf::VS_dwarf_constant;
  if (typeid (*_m_value) == typeid (value_reference))
    return dwarf::VS_reference;
  if (typeid (*_m_value) == typeid (value_unit_reference))
    return dwarf::VS_unit_reference;
  if (typeid (*_m_value) == typeid (value_lineptr))
    return dwarf::VS_lineptr;
  if (typeid (*_m_value) == typeid (value_macptr))
    return dwarf::VS_macptr;
  if (typeid (*_m_value) == typeid (value_rangelistptr))
    return dwarf::VS_rangelistptr;
  if (typeid (*_m_value) == typeid (value_identifier))
    return dwarf::VS_identifier;
  if (typeid (*_m_value) == typeid (value_string))
    return dwarf::VS_string;
  if (typeid (*_m_value) == typeid (value_source_file))
    return dwarf::VS_source_file;
  if (typeid (*_m_value) == typeid (value_source_line))
    return dwarf::VS_source_line;
  if (typeid (*_m_value) == typeid (value_source_column))
    return dwarf::VS_source_column;
  if (typeid (*_m_value) == typeid (value_address))
    return dwarf::VS_address;
  if (typeid (*_m_value) == typeid (value_constant)
      || typeid (*_m_value) == typeid (value_constant_block))
    return dwarf::VS_constant;
  if (typeid (*_m_value) == typeid (value_location))
    return dwarf::VS_location;

  throw std::runtime_error ("XXX impossible");
}

template<>
std::string
to_string<dwarf_edit::attribute> (const dwarf_edit::attribute &attr)
{
  std::string result = dwarf::attributes::name (attr.first);
  result += "=";
  result += attr.second.to_string ();
  return result;
}

std::string
dwarf_edit::source_file::to_string () const
{
  if (likely (_m_mtime == 0) && likely (_m_size == 0))
    return "\"" + _m_name + "\"";

  std::ostringstream os;
  os << "{\"" << _m_name << "," << _m_mtime << "," << _m_size << "}";
  return os.str ();
}

std::string
dwarf_edit::location_attr::to_string () const
{
  return is_list () ? "XXX-loclist" : "XXX-expr";
}
