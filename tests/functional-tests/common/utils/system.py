import os
import subprocess
import shutil
import configuration as cfg

import gobject
import glib
import dbus
from dbus.mainloop.glib import DBusGMainLoop
import time

# Don't use /tmp (not enough space there)

TEST_ENV_VARS =  { "XDG_DATA_HOME" : os.path.join (cfg.TEST_TMP_DIR, "xdg-data-home"),
                   "XDG_CACHE_HOME": os.path.join (cfg.TEST_TMP_DIR, "xdg-cache-home")} 
EXTRA_DIRS = [os.path.join (cfg.TEST_TMP_DIR, "xdg-data-home", "tracker"),
              os.path.join (cfg.TEST_TMP_DIR, "xdg-cache-home", "tracker")]

# This variable is not in the dictionary because not all tests need to modify it!
XDG_CONFIG_HOME_DIR = os.path.join (cfg.TEST_TMP_DIR, "xdg-config-home")

REASONABLE_TIMEOUT = 30

class TrackerStoreLifeCycle ():

    def __init__ (self):
        self.timeout_id = 0

    def start (self):
        """
        call this method to start and instance of tracker-store. It will return when the store is ready
        """
        self.loop = gobject.MainLoop()
        dbus_loop = DBusGMainLoop(set_as_default=True)
        self.bus = dbus.SessionBus (dbus_loop)

        obj = self.bus.get_object ("org.freedesktop.DBus",
                                   "/org/freedesktop/DBus")
        self.admin = dbus.Interface (obj, dbus_interface="org.freedesktop.DBus")
        if (self.admin.NameHasOwner (cfg.TRACKER_BUSNAME)):
            raise Exception ("Store is already running! kill it before starting this one")

        self.name_owner_match = self.bus.add_signal_receiver (self.__name_owner_changed_cb,
                                                         signal_name="NameOwnerChanged",
                                                         path="/org/freedesktop/DBus",
                                                         dbus_interface="org.freedesktop.DBus")
        self.store_proc = self.__start_tracker_store ()
        
        # It should step out of this loop when the miner is visible in DBus
        self.loop.run ()

        tracker = self.bus.get_object (cfg.TRACKER_BUSNAME, cfg.TRACKER_OBJ_PATH)
        tracker_status = self.bus.get_object (cfg.TRACKER_BUSNAME, cfg.TRACKER_STATUS_OBJ_PATH)
        self.status_iface = dbus.Interface (tracker_status, dbus_interface=cfg.STATUS_IFACE)
        print "Waiting for the store to be ready"
        self.status_iface.Wait ()
        print "Store is ready"

        
    def stop (self):
        self.__stop_tracker_store ()
        # It should step out of this loop when the miner disappear from the bus
        self.timeout_id = glib.timeout_add_seconds (REASONABLE_TIMEOUT, self.__timeout_on_idle)
        self.loop.run ()

        print "Store stopped"
        # Disconnect the signals of the next start we get duplicated messages
        self.bus._clean_up_signal_match (self.name_owner_match)

    def kill (self):
        self.store_proc.kill ()
        self.loop.run ()
        # Name owner changed cb should take us out from this loop

        print "Store killed."
        self.bus._clean_up_signal_match (self.name_owner_match)

    def __timeout_on_idle (self):
        print "Timeout (store)... asumming idle"
        self.loop.quit ()
        return False

    def __name_owner_changed_cb (self, name, old_owner, new_owner):
        if name == cfg.TRACKER_BUSNAME:
            print "Store name change %s -> %s" % (old_owner, new_owner)
            self.loop.quit ()

    def __start_tracker_store (self):
        tracker_binary = os.path.join (cfg.EXEC_PREFIX, "tracker-store")
        tracker = [tracker_binary]
        # The env variables can be passed as parameters!
        FNULL = open('/dev/null', 'w')
        return subprocess.Popen (tracker, stdout=FNULL, stderr=FNULL)

    def __stop_tracker_store (self):
        #control_binary = os.path.join (cfg.BINDIR, "tracker-control")
        #FNULL = open('/dev/null', 'w')
        #subprocess.call ([control_binary, "-t"], stdout=FNULL)
        self.store_proc.terminate ()


class TrackerMinerFsLifeCycle():
    """
    Starts and monitors the miner-fs life cycle
    """
    def __init__ (self):
        self.timeout_id = 0
    
    def start (self):
        """
        call this method to start and instance of miner-fs. It will return when the miner is 'Idle'
        after all the initial crawling
        """
        self.loop = gobject.MainLoop()
        dbus_loop = DBusGMainLoop(set_as_default=True)
        self.bus = dbus.SessionBus (dbus_loop)

        obj = self.bus.get_object ("org.freedesktop.DBus",
                                   "/org/freedesktop/DBus")
        self.admin = dbus.Interface (obj, dbus_interface="org.freedesktop.DBus")
        if (self.admin.NameHasOwner (cfg.MINERFS_BUSNAME)):
            raise Exception ("Miner is already running! kill it before starting this one")

        self.name_owner_match = self.bus.add_signal_receiver (self.__name_owner_changed_cb,
                                                         signal_name="NameOwnerChanged",
                                                         path="/org/freedesktop/DBus",
                                                         dbus_interface="org.freedesktop.DBus")
        self.__start_tracker_miner_fs ()
        
        # It should step out of this loop when the miner is visible in DBus
        self.loop.run ()

        self.status_match = self.bus.add_signal_receiver (self.__minerfs_status_cb,
                                                     signal_name="Progress",
                                                     path=cfg.MINERFS_OBJ_PATH,
                                                     dbus_interface=cfg.MINER_IFACE)
        # It should step out of this loop after to "Idle" progress changes
        self.timeout_id = glib.timeout_add_seconds (REASONABLE_TIMEOUT, self.__timeout_on_idle)
        self.loop.run ()

        
    def stop (self):
        self.__stop_tracker_miner_fs ()
        # It should step out of this loop when the miner disappear from the bus
        self.timeout_id = glib.timeout_add_seconds (REASONABLE_TIMEOUT, self.__timeout_on_idle)
        self.loop.run ()

        # Disconnect the signals of the next start we get duplicated messages
        self.bus._clean_up_signal_match (self.name_owner_match)
        self.bus._clean_up_signal_match (self.status_match)


    def wait_for_idle (self, timeout=REASONABLE_TIMEOUT):
        # The signal is already connected
        print "Waiting for Idle"
        self.timeout_id = glib.timeout_add_seconds (timeout, self.__timeout_on_idle)
        self.loop.run ()

    def __timeout_on_idle (self):
        print "Timeout... asumming idle"
        self.loop.quit ()
        return False

    def __minerfs_status_cb (self, status, handle):
        print "Miner status is now", status.encode ("utf-8")
        if (status == "Idle"):
            if (self.timeout_id != 0):
                glib.source_remove (self.timeout_id)
                self.timeout_id = 0
            self.loop.quit ()


    def __name_owner_changed_cb (self, name, old_owner, new_owner):
        if name == cfg.MINERFS_BUSNAME:
            print "Miner name change %s -> %s" % (old_owner, new_owner)
            self.loop.quit ()

    def __start_tracker_miner_fs (self):
        miner_fs_binary = os.path.join (cfg.EXEC_PREFIX, "tracker-miner-fs")
        FNULL = open ('/dev/null', 'w')
        return subprocess.Popen ([miner_fs_binary], stdout=FNULL, stderr=FNULL)

    def __stop_tracker_miner_fs (self):
        control_binary = os.path.join (cfg.BINDIR, "tracker-control")
        FNULL = open('/dev/null', 'w')
        subprocess.call ([control_binary, "-t"], stdout=FNULL)


class TrackerWritebackLifeCycle():
    """
    Starts and monitors the writeback life cycle
    """
    def __init__ (self):
        self.timeout_id = 0
    
    def start (self):
        """
        call this method to start and instance of writeback.
        It will return when the Writeback object is visible in dbus
        """
        self.loop = gobject.MainLoop()
        dbus_loop = DBusGMainLoop(set_as_default=True)
        bus = dbus.SessionBus (dbus_loop)

        obj = bus.get_object ("org.freedesktop.DBus",
                              "/org/freedesktop/DBus")
        self.admin = dbus.Interface (obj, dbus_interface="org.freedesktop.DBus")
        if (self.admin.NameHasOwner (cfg.WRITEBACK_BUSNAME)):
            raise Exception ("Writeback is already running! kill it before starting this one")

        bus.add_signal_receiver (self.__name_owner_changed_cb,
                                 signal_name="NameOwnerChanged",
                                 path="/org/freedesktop/DBus",
                                 dbus_interface="org.freedesktop.DBus")
        self.__start_tracker_writeback ()

        # It should step out of this loop when the writeback is visible in DBus
        self.timeout_id = glib.timeout_add_seconds (REASONABLE_TIMEOUT, self.__timeout_on_idle)
        self.loop.run ()
        
    def stop (self):
        assert self.process 
        self.process.kill ()

    def __name_owner_changed_cb (self, name, old_owner, new_owner):
        if name == cfg.WRITEBACK_BUSNAME:
            print "Writeback changed named '%s' -> '%s'" % (old_owner, new_owner)
            if (self.timeout_id != 0):
                glib.source_remove (self.timeout_id)
                self.timeout_id = 0
            self.loop.quit ()
            
    def __timeout_on_idle (self):
        print "Timeout... asumming idle"
        self.loop.quit ()
        return False

    def __start_tracker_writeback (self):
        writeback_binary = os.path.join (cfg.EXEC_PREFIX, "tracker-writeback")
        writeback = [writeback_binary]
        # The env variables can be passed as parameters!
        FNULL = open('/dev/null', 'w')
        self.process = subprocess.Popen (writeback, stdout=FNULL, stderr=FNULL)


class TrackerSystemAbstraction:

    def set_up_environment (self, confdir):
        """
        Sets up the XDG_*_HOME variables and make sure the directories exist
        """
        for var, directory in TEST_ENV_VARS.iteritems ():
            print "Setting %s - %s" %(var, directory)
            self.__recreate_directory (directory)
            os.environ [var] = directory

        for directory in EXTRA_DIRS:
            self.__recreate_directory (directory)
            
        if confdir :
            self.__recreate_directory (XDG_CONFIG_HOME_DIR)
            shutil.copytree (os.path.join (confdir, "tracker"),
                             os.path.join (XDG_CONFIG_HOME_DIR, "tracker"))
            print "Setting %s - %s" % ("XDG_CONFIG_HOME", XDG_CONFIG_HOME_DIR)
            print "  taking configuration from", confdir
            os.environ ["XDG_CONFIG_HOME"] = XDG_CONFIG_HOME_DIR

    def unset_up_environment (self):
        """
        Unset the XDG_*_HOME variables from the environment
        """
        for var, directory in TEST_ENV_VARS.iteritems ():
            if os.environ.has_key (var):
                del os.environ [var]
                
        if (os.environ.has_key ("XDG_CONFIG_HOME")):
            del os.environ ["XDG_CONFIG_HOME"]
        

    def tracker_store_testing_start (self, confdir=None):
        """
        Stops any previous instance of the store, calls set_up_environment,
        and starts a new instances of the store
        """
        self.__stop_tracker_processes ()
        self.set_up_environment (confdir)
        
        self.store = TrackerStoreLifeCycle ()
        self.store.start ()

    def tracker_store_start (self):
        self.store.start ()

    def tracker_store_brutal_restart (self):
        self.store.kill ()
        self.store.start ()

    def tracker_store_prepare_journal_replay (self):
        db_location = os.path.join (TEST_ENV_VARS ['XDG_CACHE_HOME'], "tracker", "meta.db")
        os.unlink (db_location)

        lockfile = os.path.join (TEST_ENV_VARS ['XDG_DATA_HOME'], "tracker", "data", ".ismeta.running")
        f = open (lockfile, 'w')
        f.write (" ")
        f.close ()

    def tracker_store_corrupt_dbs (self):
        db_path = os.path.join (TEST_ENV_VARS ['XDG_CACHE_HOME'], "tracker", "meta.db")
        f = open (db_path, "w")
        for i in range (0, 100):
            f.write ("Some stupid content... hohohoho, not a sqlite file anymore!\n")
        f.close ()

    def tracker_store_remove_journal (self):
        db_location = os.path.join (TEST_ENV_VARS ['XDG_DATA_HOME'], "tracker", "data")
        shutil.rmtree (db_location)
        os.mkdir (db_location)

    def tracker_store_testing_stop (self):
        """
        Stops a running tracker-store and unset all the XDG_*_HOME vars
        """
        assert self.store
        self.store.stop ()
        self.unset_up_environment ()


    def tracker_miner_fs_testing_start (self, confdir=None):
        """
        Stops any previous instance of the store and miner, calls set_up_environment,
        and starts a new instance of the store and miner-fs
        """
        self.__stop_tracker_processes ()
        self.set_up_environment (confdir)

        self.miner_fs = TrackerMinerFsLifeCycle ()
        self.miner_fs.start ()

    def tracker_miner_fs_wait_for_idle (self, timeout=REASONABLE_TIMEOUT):
        """
        Copy the files physically in the filesyste and wait for the miner to complete the work
        """
        self.miner_fs.wait_for_idle (timeout)
        time.sleep (1)


    def tracker_miner_fs_testing_stop (self):
        """
        Stops the miner-fs and store running and unset all the XDG_*_HOME vars
        """
        self.miner_fs.stop ()
        self.__stop_tracker_processes ()
        self.unset_up_environment ()


    def tracker_writeback_testing_start (self, confdir=None):
        # Start the miner-fs (and store) and then the writeback process
        self.tracker_miner_fs_testing_start (confdir)
        self.writeback = TrackerWritebackLifeCycle ()
        self.writeback.start ()

    def tracker_writeback_testing_stop (self):
        # Tracker write must have been started before
        self.writeback.stop ()
        self.tracker_miner_fs_testing_stop ()
        
    #
    # Private API
    #
    def __stop_tracker_processes (self):
        control_binary = os.path.join (cfg.BINDIR, "tracker-control")
        FNULL = open('/dev/null', 'w')
        subprocess.call ([control_binary, "-t"], stdout=FNULL)
        time.sleep (1)



    def __recreate_directory (self, directory):
        if (os.path.exists (directory)):
            shutil.rmtree (directory)
        os.makedirs (directory)


if __name__ == "__main__":
    import gtk, glib, time

    def destroy_the_world (a):
        a.tracker_store_testing_stop ()
        print "   stopped"
        gtk.main_quit()

    print "-- Starting store --"
    a = TrackerSystemAbstraction ()
    a.tracker_store_testing_start ()
    print "   started, waiting 5 sec. to stop it"
    glib.timeout_add_seconds (5, destroy_the_world, a)
    gtk.main ()
    
    print "-- Starting miner-fs --"
    b = TrackerMinerFsLifeCycle ()
    b.start ()
    print "  started, waiting 3 secs. to stop it"
    time.sleep (3)
    b.stop ()
    print "  stopped"
