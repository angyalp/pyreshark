/* pyreshark.c
 *
 * Pyreshark Plugin for Wireshark. (https://github.com/ashdnazg/pyreshark)
 *
 * Copyright (c) 2013 by Eshed Shaham
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

/* The following two lines prevent redefinition of ssize_t on win64*/
#define _SSIZE_T_DEFINED
#define QT_VERSION

#include "config.h"

#include "pyreshark.h"
#include "python_loader.h"


#include "param_structs.h"

#include <file.h>
#include <glib.h>

#include <epan/packet.h>
#include <epan/expert.h>

#if VERSION_MINOR > 10
#include <wsutil/filesystem.h>
#else
#include <epan/filesystem.h>
#endif

int g_num_dissectors = 0;
py_dissector_t **g_dissectors = NULL;
python_lib_t *g_python_lib;


#if VERSION_MINOR > 10
static expert_field ei_pyreshark_protocol_not_found = EI_INIT;
#endif


void
init_pyreshark(void)
{
    char *py_init_path;
    char *python_cmd;
    void *py_init_file;
    char *python_datafile_path;
    char *python_persconffile_path;
    python_version_t py_version = PYTHON_VERSION_NOT_SET;

    g_python_lib = load_python(&py_version);
    if (g_python_lib == NULL)
    {
        return;
    }

    python_datafile_path = get_datafile_path(PYTHON_DIR);
    if (NULL == python_datafile_path)
    {
        return;
    }

    python_persconffile_path = get_persconffile_path(PYTHON_DIR, 1);
    if (NULL == python_persconffile_path)
    {
        g_free(python_datafile_path);
        return;
    }

    /* Add the personal python datafile path and the global python
     * datafile path to the python path */
    python_cmd = g_strdup_printf("import sys;sys.path.append(\'%s\');sys.path.append(\'%s\')",
				 python_persconffile_path, python_datafile_path);
    g_python_lib->PyRun_SimpleStringFlags(python_cmd, NULL);
    g_free(python_cmd);
    g_free(python_datafile_path);
    g_free(python_persconffile_path);

    /* Try to load pyreshark from the personal config directory first */
    py_init_path = get_persconffile_path(PYTHON_DIR G_DIR_SEPARATOR_S PYRESHARK_INIT_FILE, 1);

    if (!g_file_test(py_init_path, G_FILE_TEST_EXISTS))
    {
        /* We could not load from the personal config directory,
         * so fall back to the global one */
        g_free(py_init_path);
        py_init_path = get_datafile_path(PYTHON_DIR G_DIR_SEPARATOR_S PYRESHARK_INIT_FILE);
    }

    py_init_file = g_python_lib->PyFile_FromString((char *)py_init_path, (char *)"rb");
    if (NULL == py_init_file)
    {
        printf("Can't open Pyreshark init file: %s\n", py_init_path);
        g_free(py_init_path);
        return;
    }
    g_free(py_init_path);

    if (py_version == PYTHON_VERSION_27)
    {
        g_python_lib->PyRun_SimpleFileExFlags(g_python_lib->PyFile_AsFile(py_init_file), PYRESHARK_INIT_FILE, FALSE, NULL);
    } else {
        g_python_lib->PyRun_SimpleFileExFlags(g_python_lib->PyFile_AsFile(py_init_file), PYRESHARK_INIT_FILE, TRUE, NULL);
    }
    g_python_lib->Py_DecRef(py_init_file);
}


void
handoff_pyreshark(void)
{
    if (g_python_lib != NULL)
    {
        g_python_lib->PyRun_SimpleStringFlags("g_pyreshark.handoff()", NULL);
    }
}

guint get_message_len(packet_info *pinfo, tvbuff_t *tvb, int offset, void *data)
{
  guint length = tvb_get_ntohs(tvb, offset);

  return length;
}

int
dissect_pyreshark(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    int i;

    for (i=0;i<g_num_dissectors; i++)
    {
        if (strcmp(g_dissectors[i]->name, pinfo->current_proto) == 0)
        {
	  if (1) // TODO: Get this from the protocol
	  {
	    tcp_dissect_pdus(tvb, pinfo, tree, TRUE,
			     8, // TODO: Get this from the protocol
			     get_message_len, // TODO: Get this from the protocol
			     dissect_proto_message, data);
	    return tvb_captured_length(tvb);
	  }
	  else
	  {
	    return dissect_proto(g_dissectors[i], tvb, pinfo, tree);
	  }
        }
    }

    if (tree)
    {
#if VERSION_MINOR > 10
        expert_add_info_format(pinfo, NULL, &ei_pyreshark_protocol_not_found,
                               "PyreShark: protocol %s not found", pinfo->current_proto);
#else
        expert_add_info_format(pinfo, NULL, PI_MALFORMED,
                    PI_ERROR, "PyreShark: protocol %s not found",
                    pinfo->current_proto);
#endif
    }
    return tvb_captured_length(tvb);
}

int
dissect_proto_message(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    int i;

    for (i=0;i<g_num_dissectors; i++)
    {
        if (strcmp(g_dissectors[i]->name, pinfo->current_proto) == 0)
        {
	    return dissect_proto(g_dissectors[i], tvb, pinfo, tree);
        }
    }

    return tvb_captured_length(tvb);
}

int
dissect_proto(py_dissector_t *py_dissector, tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
    int i;
    int offset = 0;
    tvb_and_tree_t tvb_and_tree = {tvb, tree};;

    for (i=0;i<py_dissector->length;i++)
    {
        py_dissector->dissection_chain[i]->func(&tvb_and_tree, pinfo, &offset, py_dissector->dissection_chain[i]->params);
    }

    return tvb_reported_length(tvb);
}


void
register_dissectors_array(int num_dissectors, py_dissector_t ** dissectors_array)
{
    g_num_dissectors = num_dissectors;
    g_dissectors = dissectors_array;
}


void
add_tree_item(tvb_and_tree_t *tvb_and_tree, packet_info *pinfo _U_, int *p_offset, add_tree_item_params_t *params)
{
    params->out_item = proto_tree_add_item(tvb_and_tree->tree, *(params->p_hf_index), tvb_and_tree->tvb, *p_offset, params->length, params->encoding);
}

void
add_text_item(tvb_and_tree_t *tvb_and_tree, packet_info *pinfo _U_, int *p_offset, add_text_item_params_t *params)
{
    params->out_item = proto_tree_add_none_format(tvb_and_tree->tree, *(params->p_hf_index), tvb_and_tree->tvb, *p_offset, params->length, "%s", params->text);
}

void
push_tree(tvb_and_tree_t *tvb_and_tree, packet_info *pinfo _U_, int *p_offset, push_tree_params_t *params)
{
    if (tvb_and_tree->tree)
    {
        *(params->p_start_offset) = *p_offset;
        *(params->p_old_tree) = tvb_and_tree->tree;
        tvb_and_tree->tree = proto_item_add_subtree(*(params->parent), *(params->p_index));
    }
}

void
pop_tree(tvb_and_tree_t *tvb_and_tree, packet_info *pinfo _U_, int *p_offset, pop_tree_params_t *params)
{
    if (tvb_and_tree->tree)
    {
        proto_item_set_len(tvb_and_tree->tree, *p_offset - *(params->p_start_offset));
        tvb_and_tree->tree = *(params->p_old_tree);
    }
}

void
push_tvb(tvb_and_tree_t *tvb_and_tree, packet_info *pinfo _U_, int *p_offset, push_tvb_params_t *params)
{
    guint8 *data;
    tvbuff_t *new_tvb;

    data = (guint8 *) g_malloc(params->length);
    memcpy(data, params->data, params->length);
    *(params->p_old_offset) = *p_offset;
    *(params->p_old_tvb) = tvb_and_tree->tvb;

    new_tvb = tvb_new_child_real_data(tvb_and_tree->tvb, data, params->length, params->length);
    tvb_set_free_cb(new_tvb, g_free);
    add_new_data_source(pinfo, new_tvb, params->name);

    tvb_and_tree->tvb = new_tvb;
    *p_offset = 0;
}

void
pop_tvb(tvb_and_tree_t *tvb_and_tree, packet_info *pinfo _U_, int *p_offset, pop_tvb_params_t *params)
{
    *p_offset = *(params->p_old_offset);
    tvb_and_tree->tvb = *(params->p_old_tvb);
}

void
advance_offset(tvb_and_tree_t *tvb_and_tree, packet_info *pinfo _U_ , int *p_offset, advance_offset_params_t *params)
{
    if (params->flags == OFFSET_FLAGS_READ_LENGTH || params->flags == OFFSET_FLAGS_READ_LENGTH_INCLUDING)
    {
        *p_offset += get_uint_value(tvb_and_tree->tvb, *p_offset, params->length, params->encoding);
    }
    if (params->flags == OFFSET_FLAGS_NONE || params->flags == OFFSET_FLAGS_READ_LENGTH)
    {
        *p_offset += params->length;
    }
}

void
set_column_text(tvb_and_tree_t *tvb_and_tree _U_, packet_info *pinfo, int *p_offset _U_, set_column_text_params_t *params)
{
    col_add_str(pinfo->cinfo, params->col_id, params->text);
}

void call_next_dissector(tvb_and_tree_t *tvb_and_tree, packet_info *pinfo, int *p_offset, call_next_dissector_params_t *params)
{
    char *temp_name = *(params->name);
    gint temp_length = *(params->length);
    *(params->name) = params->default_name;
    *(params->length) = params->default_length;
    call_dissector(find_dissector(temp_name), tvb_new_subset(tvb_and_tree->tvb, *p_offset, temp_length, temp_length), pinfo, tvb_and_tree->tree);
}

guint32
get_uint_value(tvbuff_t *tvb, gint offset, gint length, const guint encoding)
{
    switch (length) {

    case 1:
        return tvb_get_guint8(tvb, offset);
    case 2:
        return encoding ? tvb_get_letohs(tvb, offset)
                : tvb_get_ntohs(tvb, offset);
    case 3:
        return encoding ? tvb_get_letoh24(tvb, offset)
                : tvb_get_ntoh24(tvb, offset);
    case 4:
        return encoding ? tvb_get_letohl(tvb, offset)
                : tvb_get_ntohl(tvb, offset);
    default:
        return 0;
    }
}
