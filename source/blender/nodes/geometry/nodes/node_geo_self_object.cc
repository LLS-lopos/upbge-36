/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_self_object_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Object>("Self Object");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  params.set_output("Self Object", const_cast<Object *>(params.self_object()));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_SELF_OBJECT, "Self Object", NODE_CLASS_INPUT);
  ntype.enum_name_legacy = "SELF_OBJECT";
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_self_object_cc
