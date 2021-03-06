<?php

/**
 * @file
 * Tracks usage of embedded files.
 */

/**
 * Implements hook_field_attach_insert().
 */
function entity_embed_field_attach_insert($entity_type, $entity) {
  _entity_embed_filter_add_file_usage_from_fields($entity_type, $entity);
}

/**
 * Implements hook_field_attach_update().
 */
function entity_embed_field_attach_update($entity_type, $entity) {
  _entity_embed_filter_add_file_usage_from_fields($entity_type, $entity);
}

/**
 * Implements hook_field_attach_delete_revision().
 */
function entity_embed_field_attach_delete_revision($entity_type, $entity) {
  list($id, $vid, $bundle) = entity_extract_ids($entity_type, $entity);

  $files = entity_embed_entity_field_count_files($entity_type, $entity);

  foreach ($files as $fid => $count) {
    if ($file = file_load($fid)) {
      file_usage_delete($file, 'entity_embed', $entity_type , $id, $count);
    }
  }
}

/**
 * Implements hook_entity_delete().
 */
function entity_embed_entity_delete($entity, $type) {
  list($id, $vid, $bundle) = entity_extract_ids($type, $entity);

  db_delete('file_usage')
    ->condition('module', 'entity_embed')
    ->condition('type', $type)
    ->condition('id', $id)
    ->execute();
}

/**
 * Add file usage from file references in an entity's text fields.
 */
function _entity_embed_filter_add_file_usage_from_fields($entity_type, $entity) {
  // Track the total usage for files from all fields combined.
  $entity_files = entity_embed_entity_field_count_files($entity_type, $entity);

  list($id, $vid, $bundle) = entity_extract_ids($entity_type, $entity);

  // When an entity has revisions and then is saved again NOT as new version the
  // previous revision of the entity has be loaded to get the last known good
  // count of files. The saved data is compared against the last version
  // so that a correct file count can be created for that (the current) version
  // ID. This code may assume some things about entities that are only true for
  // node objects and should be reviewed.
  // @TODO this conditional can probably be condensed
  if (empty($entity->revision) && empty($entity->old_vid) && empty($entity->is_new) && ! empty($entity->original)) {
    $old_files = entity_embed_entity_field_count_files($entity_type, $entity->original);

    foreach ($old_files as $fid => $old_file_count) {
      // Were there more files on the node just prior to saving?
      if (empty($entity_files[$fid])) {
        $entity_files[$fid] = 0;
      }

      if ($old_file_count > $entity_files[$fid]) {
        $deprecate = $old_file_count - $entity_files[$fid];

        // Now deprecate this usage
        $file = file_load($fid);

        if ($file) {
          file_usage_delete($file, 'entity_embed', $entity_type, $id, $deprecate);
        }

        // Usage is deleted, nothing more to do with this file.
        unset($entity_files[$fid]);
      }
      // There are the same number of files, nothing to do.
      elseif ($entity_files[$fid] ==  $old_file_count) {
        unset($entity_files[$fid]);
      }
      // There are more files now, adjust the difference for the greater number.
      // file_usage incrementing will happen below.
      else {
        // We just need to adjust what the file count will account for the new
        // images that have been added since the increment process below will
        // just add these additional ones in.
        $entity_files[$fid] = $entity_files[$fid] - $old_file_count;
      }
    }
  }

  // Each entity revision counts for file usage. If versions are not enabled
  // the file_usage table will have no entries for this because of the delete
  // query above.
  foreach ($entity_files as $fid => $entity_count) {
    if ($file = file_load($fid)) {
      file_usage_add($file, 'entity_embed', $entity_type, $id, $entity_count);
    }
  }
}

/**
 * Retrieve a count of all of the files referenced by an entity.
 *
 * @param $entity
 *   The entity whose files are being counted.
 * @param $entity_type
 *   The type of the entity whose files are being counted.
 *
 * @return
 *   An array of file counts keyed by file ID.
 */
function entity_embed_entity_field_count_files($entity_type, $entity) {
  $file_count = array();

  // Loop through the entity's file references and create a total count for each
  // file.
  foreach (entity_embed_filter_parse_from_fields($entity_type, $entity) as $fid) {
    if (empty($file_count[$fid])) {
      $file_count[$fid] = 1;
    }
    else {
      $file_count[$fid]++;
    }
  }

  return $file_count;
}

/**
 * Parse file references from an entity's text fields.
 *
 * @param $entity
 *   The entity whose fields are being parsed.
 * @param $entity_type
 *   The type of the entity whose fields are being parsed.
 *
 * @return
 *   An array of file IDs corresponding to the files referenced in the fields.
 */
function entity_embed_filter_parse_from_fields($entity_type, $entity) {
  $fids = array();

  // Loop through the individual field items for all of the field instances
  // attached to the entity which support text processing and check if any of
  // them contain references to embedded files.
  foreach (entity_embed_filter_fields_with_text_filtering($entity_type, $entity) as $field_name) {
    if ($field_items = field_get_items($entity_type, $entity, $field_name)) {
      foreach ($field_items as $field_item) {
        $text = $field_item['value'];

        if (stristr($text, 'data-entity-type="file"') !== FALSE) {
          $dom = entity_embed_dom_load_html($text);
          $xpath = new \DOMXPath($dom);

          foreach ($xpath->query('//*[@data-entity-type="file" and (@data-entity-uuid or @data-entity-id)]') as $node) {
            $id = $node->getAttribute('data-entity-uuid') ?: $node->getAttribute('data-entity-id');

            $fids[] = $id;
          }
        }
      }
    }
  }

  return $fids;
}

/**
 * Retrieve a list of fields attached to an entity with text processing support.
 *
 * @param $entity
 *   The entity whose fields are being examined.
 * @param $entity_type
 *   The type of the entity whose fields are being examined.
 *
 * @return
 *   An array of the field names which support text processing.
 */
function entity_embed_filter_fields_with_text_filtering($entity_type, $entity) {
  list($id, $vid, $bundle) = entity_extract_ids($entity_type, $entity);

  // Find all of the field instances attached to the entity.
  $field_instances = field_info_instances($entity_type, $bundle);

  $fields_with_text_processing = array();

  // Loop through the field instances and build a list of the ones which support
  // text processing.
  foreach ($field_instances as $field_name => $field) {
    if (!empty($field['settings']['text_processing'])) {
      $fields_with_text_processing[] = $field_name;
    }
  }

  return $fields_with_text_processing;
}
