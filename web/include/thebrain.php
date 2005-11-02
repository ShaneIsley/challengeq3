<?php
$navlist = array(
 "home" => "Home",
 "instruc" => "Instructions",
 "status" => "Status"
);

function include_safe($file) {
	if (file_exists("include/$file")) {
		include($file);
	}
	else {
		$code = 404;
		require("errors.php");
	}
}

if (!$_GET['page']) {
	$page = "home";
}
else {
	$page = $_GET['page'];
}
?>
