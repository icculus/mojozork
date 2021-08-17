<?php

$baseurl = 'https://multizork.icculus.org';
$dbname = 'multizork.sqlite3';
$db = NULL;
$title = 'multizork';

if (!function_exists('str_ends_with')) {
    function str_ends_with($haystack, $needle) {
        return $needle !== '' && substr($haystack, -strlen($needle)) === (string)$needle;
    }
}

function print_header($subtitle)
{
    global $title;
    $str = <<<EOS
<html>
  <head>
    <meta http-equiv="Content-Type" content="text/html; charset=utf-8"/>
    <link rel="icon" type="image/ico" href="/static_files/favicon.ico" />
    <title>$title - $subtitle</title>
    <meta name="twitter:card" content="summary" />
    <meta name="twitter:site" content="@icculus" />
    <xmeta name="twitter:image" content="https://multizork.icculus.org/static_files/multizork.png" />
    <xmeta name="og:image" content="https://multizork.icculus.org/static_files/multizork.png" />
    <meta name="twitter:url" content="https://multizork.icculus.org/" />
    <meta property="og:url" content="https://multizork.icculus.org/" />
    <meta name="twitter:title" content="multizork" />
    <meta property="og:title" content="multizork" />
    <meta name="twitter:description" content="Making Zork 1 more social!" />
    <meta property="og:description" content="Making Zork 1 more social!" />
    <style>
      /* Text and background color for light mode */
      body {
        color: #333;
        max-width: 900px;
        margin: 50px;
        margin-left: auto;
        margin-right: auto;
        font-size: 16px;
        line-height: 1.3;
        font-weight: 300;
      }

      .userinput {
        color: #0000AA;
      }

      .gameoutput {
        color: #000000;
      }

      .sysmessage {
        color: #909090;
      }

      .gamecrashed {
        color: #CC0000;
      }

      /* Text and background color for dark mode */
      @media (prefers-color-scheme: dark) {
        body {
          color: #ddd;
          background-color: #222;
        }

        a {
          color: #809fff;
        }

        .gameoutput {
          color: #DDDDDD;
        }

        .userinput {
          color: #9999FF;
        }

        .sysmessage {
          color: #777777;
        }

        .gamecrashed {
          color: #FF0000;
        }
      }
    </style>
  </head>
  <body>

  <p><center>multizork! [ <a href="/">what?</a> | <a href="telnet://multizork.icculus.org">play!</a> | <a href="https://patreon.com/icculus">DONATE</a> | <a href="https://github.com/icculus/mojozork">source code</a> | <a href="https://twitter.com/icculus">twitter</a> ]</center></p><hr/>
EOS;
    print($str);
}

function print_footer()
{
    $str = <<<EOS
  </body>
</html>
EOS;
    print($str);
}

function fail($response, $msg, $url = NULL)
{
    global $title;
    header("HTTP/1.0 $response");
    if ($url != NULL) { header("Location: $url"); }
    print_header($response);
    print("<p><h1>$response</h1></p>\n\n<p>$msg</p>\n");
    print_footer();
    exit(1);
}

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
        print_header("game $hashid");
        print("<p><h1>Game instance '$hashid'</h1></p>\n");
        print("<p><ul>\n");
        print("<li>story file: '{$instancerow['story_filename']}'</li>\n");
        print("<li>number of players: {$instancerow['num_players']}</li>\n");
        print("<li>started: " . timestamp_to_string($instancerow['starttime']) . "</li>\n");
        print("<li>last saved: " . timestamp_to_string($instancerow['savetime']) . "</li>\n");
        print("<li>z-machine instructions run: {$instancerow['instructions_run']}</li>\n");
        print("<li>game crashed: " . (($instancerow['crashed'] != 0) ? "YES" : "no") . "</li>\n");
        print("<li>transcripts available for players:");

        $sawone = false;
        $stmt = $db->prepare('select * from players where instance = :instid order by id;');
        $stmt->bindValue(':instid', $instancerow['id']);
        $results = $stmt->execute();
        while ($playerrow = $results->fetchArray()) {
            if (!$sawone) {
                $sawone = true;
                print(" [");
            } else {
                print(" |");
            }
            print(" <a href='$baseurl/player/$hashid/{$playerrow['id']}'>" . htmlspecialchars($playerrow['username']) . "</a>");
        }

        if (!$sawone) {
            print(" (no transcripts found!?)");
        } else {
            print(" ]");
        }

        print("</li>\n</ul></p>\n");
        print_footer();
    } else {
        fail404("No such page");
    }
}

function display_player($hashid, $playerid, $raw)
{
    global $db, $title, $baseurl;
    $stmt = $db->prepare('select p.username, i.crashed from players as p inner join instances as i on p.instance=i.id where i.hashid = :hashid and p.id = :playerid limit 1;');
    $stmt->bindValue(':hashid', "$hashid");
    $stmt->bindValue(':playerid', "$playerid");
    $results = $stmt->execute();
    if ($row = $results->fetchArray()) {
        $crashed = $row['crashed'];
        $escuname = htmlspecialchars($row['username']);
        print_header("player $escuname - game '$hashid'");
        print("<p><h1>Transcript for player '$escuname'</h1></p>\n");
        print("<p><a href='$baseurl/game/$hashid'>[ game details</a> | ");
        if ($raw) {
            print("<a href='$baseurl/player/$hashid/$playerid'>pretty HTML version</a> ]</p>\n");
        } else {
            print("<a href='$baseurl/rawplayer/$hashid/$playerid'>raw text version</a> ]</p>\n");
        }

        $stmt = $db->prepare('select * from transcripts where player = :playerid order by id;');
        $stmt->bindValue(':playerid', $playerid);
        $results = $stmt->execute();

        if ($raw) {
            print("<pre>\n");
            while ($row = $results->fetchArray()) {
                print(htmlspecialchars($row['content']));
            }
            if ($crashed != 0) {
                print("\n\n *** GAME INSTANCE CRASHED HERE ***\n\n");
            }
            print("</pre>\n");
        } else {
            while ($row = $results->fetchArray()) {
                $texttype = $row['texttype'];
                if ($texttype == 0) {
                    $divclass = 'gameoutput';
                } else if ($texttype == 1) {
                    $divclass = 'userinput';
                } else {
                    $divclass = 'sysmessage';
                }

                $text = $row['content'];
                $fixprompt = ($texttype == 2) && str_ends_with($text, "\n>");
                if ($fixprompt) {
                    $text = substr($text, 0, strlen($text) - 1);
                }
                $esctext = str_replace("\n", "<br/>", htmlspecialchars($text));
                print("<span class='$divclass'>$esctext</span>");
                if ($fixprompt) {
                    print("<span class='gameoutput'>&gt;</span>");
                }
            }

            if ($crashed != 0) {
                print("<br/><br/><span class='gamecrashed'>*** GAME INSTANCE CRASHED HERE ***</span><br/><br/>");
            }
        }
        print_footer();
    } else {
        fail404("No such page");
    }
}

function display_mainpage()
{
    print_header("hello sailor!");
    $str = <<<EOS
<p><h1>multizork</h1></p>
<p><code>&gt;read leaflet</code></p>
<p><h2>What is this?</h2></p>
<p>This is an experiment in turning <a href="https://en.wikipedia.org/wiki/Zork">Zork 1</a> into a multiplayer game.
Up to four players can explore the Great Underground Empire at the same time, interacting with the world together 
or apart.</p>
<p>Sometimes this introduces new solutions to puzzles. Sometimes it breaks the game in hilarious ways.</p>
<p><h2>How do I play?</h2></p>
<p>You need a <a href="https://en.wikipedia.org/wiki/Telnet">telnet client</a> and some friends.</p>
<p>Telnet to multizork.icculus.org, start a game and tell your friends the access code.</p>
<p><h2>Where do I get a "telnet client"?!</h2></p>
<p>From a command line prompt, type "<code>telnet multizork.icculus.org</code>" ... you might already have one! If not:</p>
<p><ul>
  <li>On Windows: install <a href="https://www.chiark.greenend.org.uk/~sgtatham/putty/">PuTTY</a>.</li>
  <li>On Linux and macOS: just use netcat! Type "<code>nc multizork.icculus.org 23</code>"</li>
</ul></p>
<p><h2>Who did this?</h2></p>
<p>My name is Ryan. I run <a href="https://icculus.org/">icculus.org</a>. I help develop <a href="https://libsdl.org/">SDL</a>. I'm <a href="https://twitter.com/icculus">on twitter</a>.</p>
<p><h2>Can I give you money?</h2></p>
<p>You can! I build wild things like multizork for my patrons over at Patreon. If you want to support me,
please don't be afraid to throw in a few bucks and see what I do next: <a href="https://www.patreon.com/bePatron?u=157265" data-patreon-widget-type="become-patron-button">Become a Patron!</a><script async src="https://c6.patreon.com/becomePatronButton.bundle.js"></script></p>
<!--<p>...or sponsor me on GitHub:<br/><iframe src="https://github.com/sponsors/icculus/button" title="Sponsor icculus" height="35" width="116" style="border: 0;"></iframe></p>-->
<p>I also sell a game engine called <a href="https://dragonruby.org/">DragonRuby Game Toolkit</a>, which lets you build 2D games quickly and pleasantly in Ruby. One-time sales and Pro subscriptions help a lot!</p>
<p>Thanks!</p>
<p><h2>I have more questions!</h2></p>
<p>I wrote a deeply technical blog post about multizork <a href="https://www.patreon.com/posts/54997062">here</a>.
But you just hit me up <a href="https://twitter.com/icculus">on Twitter</a>
if you have more questions!</p>

EOS;
    print($str);
    print_footer();
}

// Mainline!

$db = new SQLite3($dbname, SQLITE3_OPEN_READONLY);
if ($db == NULL) {
    fail503("Couldn't access database. Please try again later.");
}

$reqargs = explode('/', preg_replace('/^\/?(.*?)\/?$/', '$1', $_SERVER['PHP_SELF']));
$reqargcount = count($reqargs);
//print_r($reqargs);

$operation = ($reqargcount >= 1) ? $reqargs[0] : '';
$document = ($reqargcount >= 2) ? $reqargs[1] : '';
$extraarg = ($reqargcount >= 3) ? $reqargs[2] : '';

if ($operation == '') {
    display_mainpage();
} else if (($operation == 'game') && ($document != '')) {
    display_instance($document);
} else if (($operation == 'player') && ($document != '') && ($extraarg != '')) {
    display_player($document, $extraarg, false);
} else if (($operation == 'rawplayer') && ($document != '') && ($extraarg != '')) {
    display_player($document, $extraarg, true);
} else {
    fail404('No such page');
}

exit(0);
?>
