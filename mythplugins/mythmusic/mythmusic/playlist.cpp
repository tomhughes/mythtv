#include <iostream>
using namespace std;
#include "playlist.h"
#include "qdatetime.h"
#include <mythtv/mythcontext.h>


Track::Track(int x, AllMusic *all_music_ptr)
{
    index_value = x;
    all_available_music = all_music_ptr;
    my_widget = NULL;
    parent = NULL;    
    bad_reference = false;
    label = QObject::tr("Not Initialized");
    cd_flag = false;
}

void Track::postLoad(PlaylistsContainer *grandparent)
{
    if(!cd_flag)
    {
        if(index_value == 0)
        {
            cerr << "playlist.o: Not sure how I got 0 as a track number, but it ain't good" << endl;
        }
        if(index_value > 0)
        {
            //  Normal Track
            label = all_available_music->getLabel(index_value, &bad_reference);
        }
        if(index_value < 0)
        {
            label = grandparent->getPlaylistName(index_value * -1, bad_reference);
        }
    }
    else
    {
        label = all_available_music->getLabel(index_value * -1, &bad_reference);
    }
}

void Track::setParent(Playlist *parent_ptr)
{
    //  I'm a track, what's my playlist?
    parent = parent_ptr;
}

void Playlist::postLoad()
{
    Track *it;
    for(it = songs.first(); it; it = songs.current())
    {
        it->postLoad(parent);
        if(it->badReference())
        {
            songs.remove(it);
            Changed();
        }
        else
        {
            it = songs.next();
        }
    }  
}

bool Playlist::checkTrack(int a_track_id)
{
    //  NB SPEED THIS UP
    //  Should be a straight lookup against cached index
    bool result = false;
    Track *it;

    for(it = songs.first(); it; it = songs.next())
    {
        if(it->getValue() == a_track_id)
        {
            result = true;
        }
    }  

    return result;    
}

void Playlist::copyTracks(Playlist *to_ptr, bool update_display)
{
    Track *it;
    for(it = songs.first(); it; it = songs.next())
    {
        if(!it->getCDFlag())
        {
            to_ptr->addTrack((*it).getValue(), update_display, false);
        }
    }  
}



void Playlist::addTrack(int the_track, bool update_display, bool cd)
{
    //  Given a track id number, add that track to 
    //  this playlist

    Track *a_track = new Track(the_track, all_available_music);
    a_track->setCDFlag(cd);
    a_track->postLoad(parent);
    a_track->setParent(this);
    songs.append(a_track);
    changed = true;

    //  If I'm part of a GUI, display the existence
    //  of this new track

    if(!update_display)
    {
        return;
    }    


    PlaylistTitle *which_widget = parent->getActiveWidget();

    if(which_widget)
    {   
        if(which_widget->childCount() > 0)
        {
            QListViewItem *first_child = which_widget->firstChild();
            QListViewItem *last_child = which_widget->firstChild();
            while((first_child = first_child->nextSibling()))
            {
                last_child = first_child;
            }
            a_track->putYourselfOnTheListView(which_widget, last_child);
        }
        else
        {
            a_track->putYourselfOnTheListView(which_widget);
        }
    }
}


void Track::deleteYourself()
{
    parent->removeTrack(index_value, cd_flag);
}

void Track::deleteYourWidget()
{
    if(my_widget)
    {
        delete my_widget;
    }
}

void Playlist::removeAllTracks()
{
    Track *it;
    for(it = songs.first(); it; it = songs.current())
    {
        it->deleteYourWidget();
        songs.remove(it);
    }  
    changed = true;
}

void Playlist::ripOutAllCDTracksNow()
{
    Track *it;
    for(it = songs.first(); it; it = songs.current())
    {
        if(it->getCDFlag())
        {
            it->deleteYourWidget();
            songs.remove(it);
        }
        else
        {
            it = songs.next();
        }
    }  
    changed = true;
}

void Playlist::removeTrack(int the_track, bool cd_flag)
{
    //  NB SPEED THIS UP
    //  Should be a straight lookup against cached index
    Track *it;
    for(it = songs.first(); it; it = songs.current())
    {
        if(it->getValue() == the_track && cd_flag == it->getCDFlag())
        {
            it->deleteYourWidget();
            songs.remove(it);
            //it = songs.last();
        }
        else
        {
            it = songs.next();
        }
    }  
    changed = true;
}

void Playlist::moveTrackUpDown(bool flag, Track *the_track)
{
    //  Slightly complicated, as the PtrList owns the pointers
    //  Need to turn off auto delete
    
    songs.setAutoDelete(false);    

    uint insertion_point = 0;
    int where_its_at = songs.findRef(the_track);
    if(where_its_at < 0)
    {
        cerr << "playlist.o: A playlist was asked to move a track, but can'd find it " << endl ;
    }
    else
    {
        if(flag)
        {
            insertion_point = ((uint) where_its_at) - 1;
        }
        else
        {
            insertion_point = ((uint) where_its_at) + 1;
        }
    
        songs.remove(the_track);
        songs.insert(insertion_point, the_track);

    }
    
    songs.setAutoDelete(true);   
    changed = true; //  This playlist is now different than Database
}

void Track::moveUpDown(bool flag)
{
    parent->moveTrackUpDown(flag, this);
}

PlaylistLoadingThread::PlaylistLoadingThread(PlaylistsContainer *parent_ptr,
                                             AllMusic *all_music_ptr)
{
    parent = parent_ptr;
    all_music = all_music_ptr;
}

void PlaylistLoadingThread::run()
{
    while(!all_music->doneLoading())
    {
        sleep(1);
    }
    parent->load();
}

void PlaylistsContainer::clearCDList()
{
    cd_playlist.clear();
}

void PlaylistsContainer::addCDTrack(int track)
{
    cd_playlist.append(track);
}

void PlaylistsContainer::removeCDTrack(int track)
{
    cd_playlist.remove(track);
}

bool PlaylistsContainer::checkCDTrack(int track)
{
    for(int i = 0; i < (int) cd_playlist.count(); i++)
    {
        if(cd_playlist[i] == track)
        {
            return true;
        }
    }    
    return false;
}

PlaylistsContainer::PlaylistsContainer(QSqlDatabase *db_ptr, AllMusic *all_music)
{
    if(!db_ptr)
    {
        cerr << "playlist.o: Tried to initialize a PlaylistsContainer with no database pointer" << endl ;
        return;
    }
    db = db_ptr;
    active_widget = NULL;
    my_host = "";
 
    active_playlist = NULL;
    backup_playlist = NULL;
    all_other_playlists = NULL;

    all_available_music = all_music;

    done_loading = false;

    playlists_loader = new PlaylistLoadingThread(this, all_music);
    playlists_loader->start();
    
    // load();  <-- this is now in the thread
}

PlaylistsContainer::~PlaylistsContainer()
{
    if (active_playlist)
        delete active_playlist;
    if (backup_playlist)
        delete backup_playlist;
    if (all_other_playlists)
        delete all_other_playlists;

    playlists_loader->wait();
    delete playlists_loader;
}

void PlaylistsContainer::load()
{
    done_loading = false;
    active_playlist = new Playlist(all_available_music);
    active_playlist->setParent(this);
    
    backup_playlist = new Playlist(all_available_music);
    backup_playlist->setParent(this);
    
    all_other_playlists = new QPtrList<Playlist>;
    all_other_playlists->setAutoDelete(true);
    
    cd_playlist.clear();



    active_playlist->loadPlaylist("default_playlist_storage", db, my_host);
    active_playlist->fillSongsFromSonglist();
    
    backup_playlist->loadPlaylist("backup_playlist_storage", db, my_host);
    backup_playlist->fillSongsFromSonglist();


    all_other_playlists->clear();

    QString aquery;
    aquery = QString(" SELECT playlistid FROM musicplaylist "
                     " WHERE name != \"default_playlist_storage\"  "
                     " AND name != \"backup_playlist_storage\"  "
                     " AND hostname = \"%1\" ORDER BY playlistid ;")
                     .arg(my_host);

    QSqlQuery query = db->exec(aquery);
    if (query.isActive() && query.numRowsAffected() > 0)
    {
        while(query.next())
        {
            Playlist *temp_playlist = new Playlist(all_available_music);   //  No, we don't destruct this ...
            temp_playlist->setParent(this);
            temp_playlist->loadPlaylistByID(query.value(0).toInt(), db, my_host);
            temp_playlist->fillSongsFromSonglist();
            all_other_playlists->append(temp_playlist); //  ... cause it's sitting on this PtrList
        }
    }
    postLoad();
    
    
    pending_writeback_index = 0;

    int x = gContext->GetNumSetting("LastMusicPlaylistPush");
    setPending(x);
    done_loading = true;
}


void PlaylistsContainer::describeYourself()
{
    //    Debugging
        
    active_playlist->describeYourself();
    Playlist *a_list;
    for(a_list = all_other_playlists->first(); a_list; a_list = all_other_playlists->next())
    {
        a_list->describeYourself();
    }

}

Playlist::Playlist(AllMusic *all_music_ptr)
{
    //  fallback values
    playlistid = 0;
    name = QObject::tr("oops");
    raw_songlist = "";
    songs.setAutoDelete(true);  //  mine!
    all_available_music = all_music_ptr;
    changed = false;
}

void Track::putYourselfOnTheListView(QListViewItem *a_listviewitem, QListViewItem *current_last_item)
{
    if(cd_flag)
    {
        my_widget = new PlaylistCD(a_listviewitem, current_last_item, label);
        my_widget->setOwner(this); 
    }
    else
    {
        if(index_value > 0)
        {
            my_widget = new PlaylistTrack(a_listviewitem, current_last_item, label);
            my_widget->setOwner(this); 
        }
        else if(index_value < 0)
        {
            my_widget = new PlaylistPlaylist(a_listviewitem, current_last_item, label);
            my_widget->setOwner(this); 
        }
    }
}

void Track::putYourselfOnTheListView(QListViewItem *a_listviewitem)
{
    if(cd_flag)
    {
        my_widget = new PlaylistCD(a_listviewitem, label);
        my_widget->setOwner(this); 
    }
    else
    {
        if(index_value > 0)
        {
            my_widget = new PlaylistTrack(a_listviewitem, label);
            my_widget->setOwner(this); 
        }
        else if(index_value < 0)
        {
            my_widget = new PlaylistPlaylist(a_listviewitem, label);
            my_widget->setOwner(this); 
        }
    }
}

void Playlist::putYourselfOnTheListView(QListViewItem *a_listviewitem)
{

    Track *it;
    PlaylistTrack *prev_track_widget = 0;
    for(it = songs.first(); it; it = songs.next())
    {
        it->putYourselfOnTheListView(a_listviewitem, prev_track_widget);
        prev_track_widget = it->getWidget();
    }  
}

int Playlist::getFirstTrackID()
{
    Track *it = songs.first();
    if(it)
    {
        return it->getValue();
    }
    return 0;
}

Playlist::~Playlist()
{
    songs.setAutoDelete(true);
    songs.clear();
}

Playlist& Playlist::operator=(const Playlist& rhs)
{
    if(this == &rhs)
    {
        return *this;
    }
    
    playlistid = rhs.playlistid;
    name = rhs.name;
    raw_songlist = rhs.raw_songlist;
    songs = rhs.songs;
    return *this;
}

void Playlist::describeYourself()
{
    //  This is for debugging
    cout << "Playlist with name of \"" << name << "\"" << endl;
    cout << "        playlistid is " << playlistid << endl;
    cout << "     songlist(raw) is \"" << raw_songlist << "\"" << endl;
    cout << "     songlist list is ";

    Track *it;
    for(it = songs.first(); it; it = songs.next())
    {
        cout << it->getValue() << "," ;
    }  

    cout << endl;
}


void Playlist::loadPlaylist(QString a_name, QSqlDatabase *a_db, QString a_host)
{
    QString thequery;
    if(a_host.length() < 1)
    {
        cerr << "playlist.o: Hey! I can't load playlists if you don't give me a hostname!" << endl; 
        return;
    }
    
    thequery = QString("SELECT playlistid, name, songlist FROM musicplaylist WHERE name = \"%1\" AND hostname = \"%2\"  ;")
                      .arg(a_name)
                      .arg(a_host);

    QSqlQuery somequery = a_db->exec(thequery);
  
    if(somequery.numRowsAffected() > 0)
    {
        while(somequery.next())
        {
            this->playlistid = somequery.value(0).toInt();
            this->name = somequery.value(1).toString();
            this->raw_songlist = somequery.value(2).toString();
        }
        if(name == "default_playlist_storage")
        {
            name = "the user should never see this";
        }
        if(name == "backup_playlist_storage")
        {
            name = "and they should **REALLY** never see this";
        }
    }
    else
    {
        // something important doesn't exist;
        cerr << "playlist.o: This is either the first time you've run mythmusic while" << endl;
        cerr << "            connected to this database, or you haven't added a recent" << endl;
        cerr << "            cvs.sql update, or something else entirely has gone horribly" << endl;
        cerr << "            wrong." << endl;
        cerr << endl;
        cerr << "            You may want to exit now and read musicdb/README." << endl;
        cerr << endl;
        name = a_name;
        saveNewPlaylist(a_db, a_host);
        changed = true;
    }
}

void Playlist::loadPlaylistByID(int id, QSqlDatabase *a_db, QString a_host)
{
    QString thequery;
    
    thequery = QString("SELECT playlistid, name, songlist FROM musicplaylist WHERE playlistid = \"%1\" AND hostname=\"%2\" ;")
                      .arg(id)
                      .arg(a_host);
    QSqlQuery somequery = a_db->exec(thequery);
  
    while(somequery.next())
    {
        this->playlistid = somequery.value(0).toInt();
        this->name = somequery.value(1).toString();
        this->raw_songlist = somequery.value(2).toString();
    }
    if(name == "default_playlist_storage")
    {
        name = "the user should never see this";
    }
    if(name == "backup_playlist_storage")
    {
        name = "and they should **REALLY** never see this";
    }
}

void Playlist::fillSongsFromSonglist()
{
    int an_int;
    QStringList list = QStringList::split(",", raw_songlist);
    QStringList::iterator it = list.begin();
    for (; it != list.end(); it++)
    {
        an_int = QString(*it).toInt();
        if(an_int != 0)
        {
            Track *a_track = new Track(an_int, all_available_music);
            a_track->setParent(this);
            songs.append(a_track);
        }
        else
        {
            changed = true;
            cerr << "playlist.o: Taking a 0 (zero) off a playlist" << endl;
            cerr << "            If this happens on repeated invocations of mythmusic, then something is really wrong" << endl;
        }
    }
}

void Playlist::fillSonglistFromSongs()
{
    bool first = true;
    QString a_list;
    Track *it;
    for(it = songs.first(); it; it = songs.next())
    {
        if(!it->getCDFlag())
        {
            if(first)
            {
                first = false;
                a_list = QString("%1").arg(it->getValue());
            }
            else
            {
                a_list += QString(QObject::tr(",%1")).arg(it->getValue());
            }
        }
    }  
    raw_songlist = a_list;
}

void Playlist::savePlaylist(QString a_name, QSqlDatabase *a_db)
{
    if(!a_db)
    {
        cerr << "playlist.o: I can't save a playlist by name without a pointer to a database" << endl ;
        return;
    }
    name = name.simplifyWhiteSpace();
    if(name.length() < 1)
    {
        return;
    }

    fillSonglistFromSongs();
    
    QString thequery  = QString("SELECT NULL FROM musicplaylist WHERE playlistid = %1 ;").arg(playlistid);

    QSqlQuery query = a_db->exec(thequery);

    if (query.isActive() && query.numRowsAffected() > 0)
    {
        thequery = QString("UPDATE musicplaylist SET songlist = \"%1\", name = \"%2\" WHERE "
                           "playlistid = \"%3\" ;")
                           .arg(raw_songlist).arg(a_name).arg(playlistid);
    }
    else
    {
        thequery = QString("INSERT musicplaylist (name,songlist) "
                           "VALUES(\"%2\",\"%1\");")
                           .arg(raw_songlist).arg(a_name);
    }
    query = a_db->exec(thequery);
}

void Playlist::saveNewPlaylist(QSqlDatabase *a_db, QString a_host)
{
    if(!a_db)
    {
        cerr << "playlist.o: I can't save a playlist by name without a pointer to a database" << endl ;
        return;
    }
    name = name.simplifyWhiteSpace();
    if(name.length() < 1)
    {
        cerr << "playlist.o: Not going to save a playlist with no name" << endl ;
        return;
    }

    if(a_host.length() < 1)
    {
        cerr << "playlist.o: Not going to save a playlist with no hostname" << endl;
        return;
    }

    fillSonglistFromSongs();
    

    QString thequery  = QString("INSERT INTO musicplaylist (name, hostname) VALUES (\"%1\", \"%2\") ;").arg(name).arg(a_host);

    QSqlQuery query = a_db->exec(thequery);

    thequery = QString("SELECT playlistid FROM musicplaylist WHERE name = \"%1\" AND hostname = \"%2\" ;").arg(name).arg(a_host);

    query = a_db->exec(thequery);
    if (query.isActive() && query.numRowsAffected() > 0)
    {
        while(query.next())
        {
            //  If multiple rows with same name,
            //  make sure we get the last one
            playlistid = query.value(0).toInt();
        }
    }
    else
    {
        cerr << "playlist.o: This is not good. Couldn't get an id back on something I just inserted" << endl ;
    }
}

int Playlist::writeTree(GenericTree *tree_to_write_to, int a_counter)
{
    Track *it;

    // compute max/min playcount,lastplay for this playlist
    int playcountMin = 0;
    int playcountMax = 0;
    double lastplayMin = 0.0;
    double lastplayMax = 0.0;
    for(it = songs.first(); it; it = songs.next())
    {
        if(!it->getCDFlag())
        {
            if(it->getValue() == 0)
            {
                cerr << "playlist.o: Oh crap ... how did we get something with an ID of 0 on a playlist?" << endl ;
            }
            if(it->getValue() > 0)
            {
                // Normal track
                Metadata *tmpdata = all_available_music->getMetadata(it->getValue());
                if (tmpdata)
                {
                    if (songs.at() == 0) { // first song
                        playcountMin = playcountMax = tmpdata->PlayCount();
                        lastplayMin = lastplayMax = tmpdata->LastPlay();
                    } else {
                        if (tmpdata->PlayCount() < playcountMin) { playcountMin = tmpdata->PlayCount(); }
                        else if (tmpdata->PlayCount() > playcountMax) { playcountMax = tmpdata->PlayCount(); }
                 
                        if (tmpdata->LastPlay() < lastplayMin) { lastplayMin = tmpdata->LastPlay(); }
                        else if (tmpdata->LastPlay() > lastplayMax) { lastplayMax = tmpdata->LastPlay(); }
                    }
                }
            }
        }
    }

    int RatingWeight = gContext->GetNumSetting("IntelliRatingWeight", 2); 
    int PlayCountWeight = gContext->GetNumSetting("IntelliPlayCountWeight", 2); 
    int LastPlayWeight = gContext->GetNumSetting("IntelliLastPlayWeight", 2); 
    int RandomWeight = gContext->GetNumSetting("IntelliRandomWeight", 2); 
    for(it = songs.first(); it; it = songs.next())
    {
        if(!it->getCDFlag())
        {
            if(it->getValue() == 0)
            {
                cerr << "playlist.o: Oh crap ... how did we get something with an ID of 0 on a playlist?" << endl ;
            }
            if(it->getValue() > 0)
            {
                // Normal track
                Metadata *tmpdata = all_available_music->getMetadata(it->getValue());
                if (tmpdata)
                {
                    QString a_string = QString(QObject::tr("%1 ~ %2")).arg(tmpdata->Artist()).arg(tmpdata->Title());
                    GenericTree *added_node = tree_to_write_to->addNode(a_string, it->getValue(), true);
                    ++a_counter;
                    added_node->setAttribute(0, 1);
                    added_node->setAttribute(1, a_counter); //  regular order
                    added_node->setAttribute(2, rand()); //  random order
                    
                    //
                    //  Compute "intelligent" weighting
                    //

                    int rating = tmpdata->Rating();
                    int playcount = tmpdata->PlayCount();
                    double lastplaydbl = tmpdata->LastPlay();
                    double ratingValue = (double)(rating) / 10;
                    double playcountValue, lastplayValue;
                    if (playcountMax == playcountMin) { playcountValue = 0; }
                    else { playcountValue = ((playcountMin - (double)playcount) / (playcountMax - playcountMin) + 1); }
                    if (lastplayMax == lastplayMin) { lastplayValue = 0; }
                    else { lastplayValue = ((lastplayMin - lastplaydbl) / (lastplayMax - lastplayMin) + 1); }
                    double rating_value =  (RatingWeight * ratingValue + PlayCountWeight * playcountValue + 
                                            LastPlayWeight * lastplayValue + RandomWeight * (double)rand() / 
                                            (RAND_MAX + 1.0));
                    int integer_rating = (int) (4000001 - rating_value * 10000);
                    added_node->setAttribute(3, integer_rating); //  "intelligent" order
					 }
            }
            if(it->getValue() < 0)
            {
                // it's a playlist, recurse (mildly)
                Playlist *level_down = parent->getPlaylist((it->getValue()) * -1);
                if (level_down)
                {
                    a_counter = level_down->writeTree(tree_to_write_to, a_counter);
                }
            }
        }
        else
        {
            //
            //  f'ing CD tracks to keep Isaac happy
            //

            Metadata *tmpdata = all_available_music->getMetadata(it->getValue() * -1);
            if (tmpdata)
            {
                QString a_string = QString(QObject::tr("%1 ~ %2")).arg(tmpdata->Artist()).arg(tmpdata->Title());
                if(tmpdata->Artist().length() < 1 ||
                   tmpdata-> Title().length() < 1)
                {
                    a_string = QString("%1 ~ Unknown").arg(tmpdata->Track());
                }
                GenericTree *added_node = tree_to_write_to->addNode(a_string, it->getValue() * -1, true);
                ++a_counter;
                added_node->setAttribute(0, 1);
                added_node->setAttribute(1, a_counter); //  regular order
                added_node->setAttribute(2, rand()); //  random order
                added_node->setAttribute(3, rand()); //  "intelligent" order
            }
        }
    }  
    return a_counter;
}

void PlaylistsContainer::writeTree(GenericTree *tree_to_write_to)
{
    all_available_music->writeTree(tree_to_write_to);

    GenericTree *sub_node = tree_to_write_to->addNode(QObject::tr("All My Playlists"), 1);
    sub_node->setAttribute(0, 1);
    sub_node->setAttribute(1, 1);
    sub_node->setAttribute(2, 1);
    sub_node->setAttribute(3, 1);
    
    GenericTree *subsub_node = sub_node->addNode(QObject::tr("Active Play Queue"), 0);
    subsub_node->setAttribute(0, 0);
    subsub_node->setAttribute(1, 0);
    subsub_node->setAttribute(2, rand());
    subsub_node->setAttribute(3, rand());

    active_playlist->writeTree(subsub_node, 0);

    int a_counter = 0;  
    
    //
    //  Write the CD playlist (if there's anything in it)
    //
    
/*
    if(cd_playlist.count() > 0)
    {
        ++a_counter;
        QString a_string = QObject::tr("CD: ");
        a_string += all_available_music->getCDTitle();
        GenericTree *cd_node = sub_node->addNode(a_string, 0);
        cd_node->setAttribute(0, 0);
        cd_node->setAttribute(1, a_counter);
        cd_node->setAttribute(2, rand());
        cd_node->setAttribute(3, rand());
        
    }
*/

    //
    //  Write the other playlists
    //
    
    QPtrListIterator<Playlist> iterator( *all_other_playlists );  
    Playlist *a_list;
    while( ( a_list = iterator.current() ) != 0)
    {
        ++a_counter;
        GenericTree *new_node = sub_node->addNode(a_list->getName(), 0);
        new_node->setAttribute(0, 0);
        new_node->setAttribute(1, a_counter);
        new_node->setAttribute(2, rand());
        new_node->setAttribute(3, rand());
        a_list->writeTree(new_node, 0);
        ++iterator;
    }
    
}

void PlaylistsContainer::save()
{
    Playlist *a_list;

    for(a_list = all_other_playlists->first(); a_list; a_list = all_other_playlists->next())
    {
        if(a_list->hasChanged())
        {
            a_list->fillSonglistFromSongs();
            a_list->savePlaylist(a_list->getName(), db);
        }
    }
    
    active_playlist->savePlaylist("default_playlist_storage", db);
    backup_playlist->savePlaylist("backup_playlist_storage", db);
}

void PlaylistsContainer::createNewPlaylist(QString name)
{
    Playlist *new_list = new Playlist(all_available_music);
    new_list->setParent(this);
    new_list->setName(name);
    
    //  Need to touch the database to get persistent ID
    new_list->saveNewPlaylist(db, my_host);
    new_list->Changed();
    all_other_playlists->append(new_list);
    //if(my_widget)
    //{
    //    new_list->putYourselfOnTheListView(my_widget);
    //}
}

void PlaylistsContainer::copyNewPlaylist(QString name)
{
    Playlist *new_list = new Playlist(all_available_music);
    new_list->setParent(this);
    new_list->setName(name);
    //  Need to touch the database to get persistent ID
    new_list->saveNewPlaylist(db, my_host);
    new_list->Changed();
    all_other_playlists->append(new_list);
    active_playlist->copyTracks(new_list, false);
    pending_writeback_index = 0;
    active_widget->setText(0, QObject::tr("Active Play Queue"));
    active_playlist->removeAllTracks();
    active_playlist->addTrack(new_list->getID() * -1, true, false);
}

void PlaylistsContainer::setActiveWidget(PlaylistTitle *widget)
{
    active_widget = widget;
    if(active_widget && pending_writeback_index > 0)
    {
        bool bad = false;
        QString newlabel = QString(QObject::tr("Active Play Queue (%1)")).arg(getPlaylistName(pending_writeback_index, bad));
        active_widget->setText(0, newlabel);
    }    
}

void PlaylistsContainer::popBackPlaylist()
{
    Playlist *destination = getPlaylist(pending_writeback_index);
    if (!destination)
    {
        cerr << "Unknown playlist: " << pending_writeback_index << endl;
        return;
    }
    destination->removeAllTracks();
    destination->Changed();
    active_playlist->copyTracks(destination, false);
    active_playlist->removeAllTracks();
    backup_playlist->copyTracks(active_playlist, true);
    pending_writeback_index = 0;
    active_widget->setText(0, QObject::tr("Active Play Queue"));

    active_playlist->Changed();
    backup_playlist->Changed();
}

void PlaylistsContainer::copyToActive(int index)
{
    backup_playlist->removeAllTracks();
    active_playlist->copyTracks(backup_playlist, false);

    pending_writeback_index = index;
    if(active_widget)
    {
        bool bad = false;
        QString newlabel = QString(QObject::tr("Active Play Queue (%1)")).arg(getPlaylistName(index, bad));
        active_widget->setText(0, newlabel);
    }    
    active_playlist->removeAllTracks();
    Playlist *copy_from = getPlaylist(index);
    if (!copy_from)
    {
        cerr << "Unknown playlist: " << index << endl;
        return;
    }
    copy_from->copyTracks(active_playlist, true);

    active_playlist->Changed();
    backup_playlist->Changed();

}


void PlaylistsContainer::renamePlaylist(int index, QString new_name)
{
    Playlist *list_to_rename = getPlaylist(index);
    if(list_to_rename)
    {
        list_to_rename->setName(new_name);
        list_to_rename->Changed();
        if(list_to_rename->getID() == pending_writeback_index)
        {
            QString newlabel = QString(QObject::tr("Active Play Queue (%1)")).arg(new_name);
            active_widget->setText(0, newlabel);
        }
    }
}

void PlaylistsContainer::deletePlaylist(int kill_me)
{
    Playlist *list_to_kill = getPlaylist(kill_me);
    if (!list_to_kill)
    {
        cerr << "Unknown playlist: " << kill_me << endl;
        return;
    }
    //  First, we need to take out any **track** on any other
    //  playlist that is actually a reference to this
    //  playlist
    
    active_playlist->removeTrack(kill_me * -1, false);

    QPtrListIterator<Playlist> iterator( *all_other_playlists );    
    Playlist *a_list;
    while( ( a_list = iterator.current() ) != 0)
    {
        ++iterator;
        if(a_list != list_to_kill)
        {
            a_list->removeTrack(kill_me * -1, false);
        }
    }

    QString aquery = QString("DELETE FROM musicplaylist WHERE playlistid = %1 ;").arg(kill_me);
    QSqlQuery query = db->exec(aquery);
    if(query.numRowsAffected() < 1)
    {
        cerr << "playlist.o: Hmmm, that's odd ... I tried to delete a playlist but the database couldn't find it" << endl ;
    }
    list_to_kill->removeAllTracks();
    all_other_playlists->remove(list_to_kill);
}


QString PlaylistsContainer::getPlaylistName(int index, bool &reference)
{
    if(active_playlist)
    {
        if(active_playlist->getID() == index)
        {
            return active_playlist->getName();
        }

        Playlist *a_list;
        for(a_list = all_other_playlists->last(); a_list; a_list = all_other_playlists->prev())
        {
            if(a_list->getID() == index)
            {
                return a_list->getName();   
            }
        }
    }
    cerr << "playlist.o: Asked to getPlaylistName() with an index number I couldn't find" << endl ;
    reference = true;
    return QObject::tr("Something is Wrong");
}

bool Playlist::containsReference(int to_check, int depth)
{
    if(depth > 10)
    {
        cerr << "playlist.o: I'm recursively checking playlists, and have reached a search depth over 10 " << endl ;
    }
    bool ref_exists = false;

    int check;
    //  Attempt to avoid infinite recursion
    //  in playlists (within playlists) =P

    Track *it;
    for(it = songs.first(); it; it = songs.next())
    {
        check = it->getValue();
        if(check < 0)
        {
            if(check * -1 == to_check)
            {
                ref_exists = true;
                return ref_exists;
            }
            else
            {
                //  Recurse down one level
                int new_depth = depth + 1;
                Playlist *new_check = parent->getPlaylist(check * -1);
                if (new_check)
                    ref_exists = new_check->containsReference(to_check, 
                                                              new_depth);
            }
        }
    }  
    return ref_exists;
}

Playlist *PlaylistsContainer::getPlaylist(int id)
{
    //  return a pointer to a playlist
    //  by id;
    
    if(active_playlist->getID() == id)
    {
        return active_playlist;
    }

    //  Very Suprisingly, we need to use an iterator on this,
    //  because if we just traverse the list, other functions (that
    //  called this) that are also traversing the same list get
    //  reset. That took a **long** time to figure out

    QPtrListIterator<Playlist> iterator( *all_other_playlists );    
    Playlist *a_list;
    while( ( a_list = iterator.current() ) != 0)
    {
        ++iterator;
        if(a_list->getID() == id)
        {
            return a_list;
        }
    }
    cerr << "playlists.o: Something asked me to find a Playlist object with an id I couldn't find" << endl ;
    return NULL;    
}

void PlaylistsContainer::showRelevantPlaylists(TreeCheckItem *alllists)
{

    QString templevel, temptitle;
    int id;
    //  Deleting anything that's there
    if(alllists->childCount() > 0)
    {
        QListViewItem *first_child;
        while((first_child = alllists->firstChild()))
        {
            delete first_child;
        }
    }

    TreeCheckItem *after = NULL;

    //  Add everything but the current playlist
    //
    //  Work around for people using Qt < 3.1, but list my get ordered
    //  differently    
    Playlist *some_list;
    for(some_list = all_other_playlists->first(); some_list; some_list = all_other_playlists->next() )
    {
        id = some_list->getID() * -1 ;
        temptitle = some_list->getName();
        templevel = "playlist";
#if (QT_VERSION >= 0x030100)
        TreeCheckItem *some_item = new TreeCheckItem(alllists, after, temptitle, templevel, id);
#else
#warning
#warning ***   You should think seriously about upgrading your Qt to 3.1.0 or higher   ***
#warning
        TreeCheckItem *some_item = new TreeCheckItem(alllists, temptitle, templevel, id);
#endif
        after = some_item;
        some_item->setCheckable(true);
        if( some_list->containsReference(pending_writeback_index, 0) ||
            (id * -1) == pending_writeback_index)
        {
            some_item->setCheckable(false);
        }
        some_list->putYourselfOnTheListView(some_item);
    }
}

void PlaylistsContainer::refreshRelevantPlaylists(TreeCheckItem *alllists)
{

    QListViewItem *walker = alllists->firstChild();
    while(walker)
    {
        if(TreeCheckItem *check_item = dynamic_cast<TreeCheckItem*>(walker) )
        {
            int id = check_item->getID() * -1;
            Playlist *check_playlist = getPlaylist(id);
            if((check_playlist && 
                check_playlist->containsReference(pending_writeback_index, 0)) ||
               id == pending_writeback_index)
            {
                check_item->setCheckable(false);
            }
            else
            {
                check_item->setCheckable(true);
            }
        }
        walker = walker->nextSibling();
    }
}


void PlaylistsContainer::postLoad()
{
    //  Now that everything is loaded, we need to recheck all
    //  tracks and update those that refer to a playlist

    active_playlist->postLoad();
    backup_playlist->postLoad();
    QPtrListIterator<Playlist> iterator( *all_other_playlists );    
    Playlist *a_list;
    while( ( a_list = iterator.current() ) != 0)
    {
        ++iterator;
        a_list->postLoad();
    }
}

bool PlaylistsContainer::pendingWriteback()
{
    if(pending_writeback_index > 0)
    {
        return true;
    }
    return false;
}

bool PlaylistsContainer::nameIsUnique(QString a_name, int which_id)
{
    if(a_name == "default_playlist_storage")
    {
        return false;
    }
    
    if(a_name == "backup_playlist_storage")
    {
        return false;
    }

    QPtrListIterator<Playlist> iterator( *all_other_playlists );    
    Playlist *a_list;
    while( ( a_list = iterator.current() ) != 0)
    {
        ++iterator;
        if(a_list->getName() == a_name &&
           a_list->getID() != which_id)
        {
            return false;
        }
    }
    return true ;
}

bool PlaylistsContainer::cleanOutThreads()
{
    if(playlists_loader->finished())
    {
        return true;
    }
    playlists_loader->wait();
    return false;
}

void PlaylistsContainer::clearActive()
{
    backup_playlist->removeAllTracks();
    active_playlist->removeAllTracks();
    backup_playlist->Changed();
    active_playlist->Changed();
    pending_writeback_index = 0;
    active_widget->setText(0, QObject::tr("Active Play Queue"));
}

