<?php

$baseurl = '/';
$dbname = 'multizork.sqlite3';
$db = NULL;
$title = 'multizork';

function fail($response, $msg, $url = NULL)
{
    global $title;
    header("HTTP/1.0 $response");
    if ($url != NULL) { header("Location: $url"); }
    header('Content-Type: text/html; charset=utf-8');
    print("<html><head><title>$title</title></head><body>\n<p><h1>$response</h1></p>\n\n<p>$msg</p>\n\n</body></html>\n");
    exit(1);
}

function fail400($msg) { fail('400 Bad Request', $msg); }
function fail404($msg) { fail('404 Not Found', $msg); }
function fail503($msg) { fail('503 Service Unavailable', $msg); }

function timestamp_to_string($t)
{
    return strftime('%D %T %Z', $t);
}

function display_instance($hashid)
{
    global $db, $title, $baseurl;
    $stmt = $db->prepare('select * from instances where hashid = :hashid limit 1;');
    $stmt->bindValue(':hashid', "$hashid");
    $results = $stmt->execute();
    if ($instancerow = $results->fetchArray()) {
        print("<html><head><title>$title - game $hashid</title></head><body>\n");
        print("<p><h1>Game instance '$hashid'</h1></p>\n");
        print("<p><ul>\n");
        print("<li>story file: '{$instancerow['story_filename']}'</li>\n");
        print("<li>number of players: {$instancerow['num_players']}</li>\n");
        print("<li>started: " . timestamp_to_string($instancerow['starttime']) . "</li>\n");
        print("<li>last saved: " . timestamp_to_string($instancerow['savetime']) . "</li>\n");
        print("<li>Z-Machine instructions run: {$instancerow['instructions_run']}</li>\n");
        print("<li>Transcripts available for players:");

        $sawone = false;
        $stmt = $db->prepare('select * from players where instance = :instid order by id;');
        $stmt->bindValue(':instid', $instancerow['id']);
        $results = $stmt->execute();
        while ($playerrow = $results->fetchArray()) {
            $sawone = true;
            print(" [<a href='$baseurl/player/{$playerrow['hashid']}'>" . htmlspecialchars($playerrow['username']) . "</a>]");
        }

        if (!$sawone) {
            print(" (no transcripts found!?)");
        }

        print("</li>\n</ul></p>\n");
        print("</body></html>\n\n");
    } else {
        fail404("No such page");
    }
}

function display_player($hashid)
{
    global $db, $title, $baseurl;
    $stmt = $db->prepare('select p.id, p.instance, p.username, i.hashid from players as p inner join instances as i on p.instance=i.id where p.hashid = :hashid limit 1;');
    $stmt->bindValue(':hashid', "$hashid");
    $results = $stmt->execute();
    if ($row = $results->fetchArray()) {
        print("<html><head><title>$title - player $hashid</title></head><body>\n");
        print("<p><h1>Transcript for player '" . htmlspecialchars($row['username']) . "'</h1></p>\n");
        print("<p>Details on this run of the game: <a href='$baseurl/game/{$row['hashid']}'>[instance {$row['hashid']}]</a></p>\n");
print("<pre>\n");
        $stmt = $db->prepare('select * from transcripts where player = :playerid order by id;');
        $stmt->bindValue(':playerid', $row['id']);
        $results = $stmt->execute();
        while ($row = $results->fetchArray()) {
            print(htmlspecialchars($row['content']));
        }
print("</pre>\n");
        print("</body></html>\n\n");
    } else {
        fail404("No such page");
    }
}


// Mainline!

$db = new SQLite3($dbname, SQLITE3_OPEN_READONLY);
if ($db == NULL) {
    fail503("Couldn't access database. Please try again later.");
}

$reqargs = explode('/', preg_replace('/^\/?(.*?)\/?$/', '$1', $_SERVER['PHP_SELF']));
array_shift($reqargs);
array_shift($reqargs);
$reqargcount = count($reqargs);
//print_r($reqargs);

$operation = ($reqargcount >= 1) ? $reqargs[0] : '';
$document = ($reqargcount >= 2) ? $reqargs[1] : '';

if ($operation == '') {
    fail503("write me"); // !!! FIXME: show the main page
} else if (($operation == 'game') && ($document != '')) {
    display_instance($document);
} else if (($operation == 'player') && ($document != '')) {
    display_player($document);
} else {
    fail404('No such page');
}

exit(0);
?>
