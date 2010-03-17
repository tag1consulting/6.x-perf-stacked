<?php
// $Id: path.inc,v 1.19.2.3 2010/03/01 09:19:15 goba Exp $

/**
 * @file
 * Functions to handle paths in Drupal, including path aliasing.
 *
 * These functions are not loaded for cached pages, but modules that need
 * to use them in hook_init() or hook exit() can make them available, by
 * executing "drupal_bootstrap(DRUPAL_BOOTSTRAP_PATH);".
 */

/**
 * Initialize the $_GET['q'] variable to the proper normal path.
 */
function drupal_init_path() {
  if (!empty($_GET['q'])) {
    $_GET['q'] = drupal_get_normal_path(trim($_GET['q'], '/'));
  }
  else {
    $_GET['q'] = drupal_get_normal_path(variable_get('site_frontpage', 'node'));
  }
}

/**
 * Given an alias, return its Drupal system URL if one exists. Given a Drupal
 * system URL return one of its aliases if such a one exists. Otherwise,
 * return FALSE.
 *
 * @param $action
 *   One of the following values:
 *   - wipe: delete the alias cache.
 *   - alias: return an alias for a given Drupal system path (if one exists).
 *   - source: return the Drupal system URL for a path alias (if one exists).
 * @param $path
 *   The path to investigate for corresponding aliases or system URLs.
 * @param $path_language
 *   Optional language code to search the path with. Defaults to the page language.
 *   If there's no path defined for that language it will search paths without
 *   language.
 *
 * @return
 *   Either a Drupal system path, an aliased path, or FALSE if no path was
 *   found.
 */
function drupal_lookup_path($action, $path = '', $path_language = '') {
  global $language;
  $cache = &drupal_static(__FUNCTION__, array(
    'map' => array(),
    'no_src' => array(),
    'whitelist' => NULL,
    'system_paths' => array(),
    'no_aliases' => array(),
    'first_call' => TRUE,
  ));

  // Retrieve the path alias whitelist.
  if (!isset($cache['whitelist'])) {
    $cache['whitelist'] = variable_get('path_alias_whitelist', NULL);
    if (!isset($cache['whitelist'])) {
      $cache['whitelist'] = drupal_path_alias_whitelist_rebuild();
    }
  }

  $path_language = $path_language ? $path_language : $language->language;

  if ($action == 'wipe') {
    $cache = array();
    $cache['whitelist'] = drupal_path_alias_whitelist_rebuild();
  }
  elseif ($cache['whitelist'] && $path != '') {
    if ($action == 'alias') {
      // During the first call to drupal_lookup_path() per language, load the
      // expected system paths for the page from cache.
      if (!empty($cache['first_call'])) {
        $cache['first_call'] = FALSE;

        if (!isset($cache['map'][$path_language]) || !is_array($cache['map'][$path_language])) {
          $cache['map'][$path_language] = array();
        }
        // Load system paths from cache.
        $cid = $_GET['q'];
        if ($cached = cache_get($cid, 'cache_path')) {
          $cache['system_paths'] = $cached->data;
          // Now fetch the aliases corresponding to these system paths.
          // We order by ASC and overwrite array keys to ensure the correct
          // alias is used when there are multiple aliases per path.
          $placeholders = db_placeholders($cache['system_paths'], 'varchar');
          $result = db_query("SELECT src, dst FROM {url_alias} WHERE src IN($placeholders) AND (language = '%s' OR language = '') ORDER BY language ASC", $cache['system_paths'], $path_language);
          while ($record = db_fetch_object($result)) {
            if (!isset($cache['map'][$path_language][$record->src])) {
              $cache['map'][$path_language][$record->src] = $record->dst;
            }
          }
          // Keep a record of paths with no alias to avoid querying twice.
          $cache['no_aliases'][$path_language] = array_flip(array_diff_key($cache['system_paths'], array_keys($cache['map'][$path_language])));
        }
      }
      // If the alias has already been loaded, return it.
      if (isset($cache['map'][$path_language][$path])) {
        return $cache['map'][$path_language][$path];
      }
      // Check the path whitelist, if the top_level part before the first /
      // is not in the list, then there is no need to do anything further,
      // it is not in the database.
      elseif (!isset($cache['whitelist'][strtok($path, '/')])) {
        return FALSE;
      }
      // For system paths which were not cached, query aliases individually.
      else if (!isset($cache['no_aliases'][$path_language][$path])) {
        // Get the most fitting result falling back with alias without language
        $alias = db_result(db_query("SELECT dst FROM {url_alias} WHERE src = '%s' AND (language = '%s' OR language = '') ORDER BY language DESC, pid DESC", $path, $path_language));
        $map[$path_language][$path] = $alias;
        return $alias;
      }
    }
    // Check no_src for this $path in case we've already determined that there
    // isn't a path that has this alias
    elseif ($action == 'source' && !isset($cache['no_src'][$path_language][$path])) {
      // Look for the value $path within the cached map
      $src = '';
      if (!isset($cache['map'][$path_language]) || !($src = array_search($path, $cache['map'][$path_language]))) {
        // Get the most fitting result falling back with alias without language
        if ($src = db_result(db_query("SELECT src FROM {url_alias} WHERE dst = '%s' AND (language = '%s' OR language =  '') ORDER BY language DESC, pid DESC", $path, $path_language))) {
          $map[$path_language][$src] = $path;
        }
        else {
          // We can't record anything into map because we do not have a valid
          // index and there is no need because we have not learned anything
          // about any Drupal path. Thus cache to no_src.
          $cache['no_src'][$path_language][$path] = TRUE;
        }
      }
      return $src;
    }
  }

  return FALSE;
}

/**
 * Cache system paths for a page.
 *
 * Cache an array of the system paths available on each page. We assume
 * that aiases will be needed for the majority of these paths during
 * subsequent requests, and load them in a single query during
 * drupal_lookup_path().
 */
function drupal_cache_system_paths() {
  // Check if the system paths for this page were loaded from cache in this
  // request to avoid writing to cache on every request.
  $cache = &drupal_static('drupal_lookup_path', array());
  if (!$cache['system_paths']) {
    // Generate a cache ID (cid) specifically for this page.
    $cid = $_GET['q'];
    // The static $map array used by drupal_lookup_path() includes all
    // system paths for the page request.
    if ($paths = current($cache['map'])) {
      $data = array_keys($paths);
      $expire = $_SERVER['REQUEST_TIME'] + (60 * 60 * 24);
      cache_set($cid, $data, 'cache_path', $expire);
    }
  }
}

/**
 * Given an internal Drupal path, return the alias set by the administrator.
 *
 * @param $path
 *   An internal Drupal path.
 * @param $path_language
 *   An optional language code to look up the path in.
 *
 * @return
 *   An aliased path if one was found, or the original path if no alias was
 *   found.
 */
function drupal_get_path_alias($path, $path_language = '') {
  $result = $path;
  if ($alias = drupal_lookup_path('alias', $path, $path_language)) {
    $result = $alias;
  }
  return $result;
}

/**
 * Given a path alias, return the internal path it represents.
 *
 * @param $path
 *   A Drupal path alias.
 * @param $path_language
 *   An optional language code to look up the path in.
 *
 * @return
 *   The internal path represented by the alias, or the original alias if no
 *   internal path was found.
 */
function drupal_get_normal_path($path, $path_language = '') {
  $result = $path;
  if ($src = drupal_lookup_path('source', $path, $path_language)) {
    $result = $src;
  }
  if (function_exists('custom_url_rewrite_inbound')) {
    // Modules may alter the inbound request path by reference.
    custom_url_rewrite_inbound($result, $path, $path_language);
  }
  return $result;
}

/**
 * Return a component of the current Drupal path.
 *
 * When viewing a page at the path "admin/content/types", for example, arg(0)
 * would return "admin", arg(1) would return "content", and arg(2) would return
 * "types".
 *
 * Avoid use of this function where possible, as resulting code is hard to read.
 * Instead, attempt to use named arguments in menu callback functions. See the
 * explanation in menu.inc for how to construct callbacks that take arguments.
 *
 * @param $index
 *   The index of the component, where each component is separated by a '/'
 *   (forward-slash), and where the first component has an index of 0 (zero).
 *
 * @return
 *   The component specified by $index, or NULL if the specified component was
 *   not found.
 */
function arg($index = NULL, $path = NULL) {
  static $arguments;

  if (!isset($path)) {
    $path = $_GET['q'];
  }
  if (!isset($arguments[$path])) {
    $arguments[$path] = explode('/', $path);
  }
  if (!isset($index)) {
    return $arguments[$path];
  }
  if (isset($arguments[$path][$index])) {
    return $arguments[$path][$index];
  }
}

/**
 * Get the title of the current page, for display on the page and in the title bar.
 *
 * @return
 *   The current page's title.
 */
function drupal_get_title() {
  $title = drupal_set_title();

  // during a bootstrap, menu.inc is not included and thus we cannot provide a title
  if (!isset($title) && function_exists('menu_get_active_title')) {
    $title = check_plain(menu_get_active_title());
  }

  return $title;
}

/**
 * Set the title of the current page, for display on the page and in the title bar.
 *
 * @param $title
 *   Optional string value to assign to the page title; or if set to NULL
 *   (default), leaves the current title unchanged.
 *
 * @return
 *   The updated title of the current page.
 */
function drupal_set_title($title = NULL) {
  static $stored_title;

  if (isset($title)) {
    $stored_title = $title;
  }
  return $stored_title;
}

/**
 * Check if the current page is the front page.
 *
 * @return
 *   Boolean value: TRUE if the current page is the front page; FALSE if otherwise.
 */
function drupal_is_front_page() {
  // As drupal_init_path updates $_GET['q'] with the 'site_frontpage' path,
  // we can check it against the 'site_frontpage' variable.
  return $_GET['q'] == drupal_get_normal_path(variable_get('site_frontpage', 'node'));
}

/**
 * Check if a path matches any pattern in a set of patterns.
 *
 * @param $path
 *   The path to match.
 * @param $patterns
 *   String containing a set of patterns separated by \n, \r or \r\n.
 *
 * @return
 *   1 if there is a match, 0 if there is not a match.
 */
function drupal_match_path($path, $patterns) {
  static $regexps;

  if (!isset($regexps[$patterns])) {
    $regexps[$patterns] = '/^('. preg_replace(array('/(\r\n?|\n)/', '/\\\\\*/', '/(^|\|)\\\\<front\\\\>($|\|)/'), array('|', '.*', '\1'. preg_quote(variable_get('site_frontpage', 'node'), '/') .'\2'), preg_quote($patterns, '/')) .')$/';
  }
  return preg_match($regexps[$patterns], $path);
}

/**
 * Central static variable storage.
 *
 * @param $name
 *   Globally unique name for the variable. For a function with only one static,
 *   variable, the function name (e.g. via the PHP magic __FUNCTION__ constant)
 *   is recommended. For a function with multiple static variables add a
 *   distinguishing suffix to the function name for each one.
 * @param $default_value
 *   Optional default value.
 * @param $reset
 *   TRUE to reset a specific named variable, or all variables if $name is NULL.
 *   Resetting every variable should only be used, for example, for running
 *   unit tests with a clean environment. Should be used only though via
 *   function drupal_static_reset().
 *
 * @return
 *   Returns a variable by reference if $reset is FALSE.
 */
function &drupal_static($name, $default_value = NULL, $reset = FALSE) {
  static $data = array();

  // Reset a single value, or all values.
  if ($reset) {
    if (isset($name)) {
      unset($data[$name]);
    }
    else {
      $data = array();
    }
    // We must return a reference to a variable.
    $dummy = NULL;
    return $dummy;
  }

  if (!isset($data[$name])) {
    $data[$name] = $default_value;
  }

  return $data[$name];
}

/**
 * Rebuild the path alias white list.
 *
 * @return
 *   An array containing a white list of path aliases.
 */
function drupal_path_alias_whitelist_rebuild() {
  // For each alias in the database, get the top level component of the system
  // path it corresponds to. This is the portion of the path before the first /
  // if present, otherwise the whole path itself.
  $whitelist = array();
  $result = db_query("SELECT SUBSTRING_INDEX(src, '/', 1) AS path FROM {url_alias} GROUP BY path");
  while ($row = db_fetch_object($result)) {
    $whitelist[$row->path] = TRUE;
  }
  variable_set('path_alias_whitelist', $whitelist);
  return $whitelist;
}
