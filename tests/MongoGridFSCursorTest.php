<?php
/**
 * Test class for Mongo.
 * Generated by PHPUnit on 2009-04-09 at 18:09:02.
 */
class MongoGridFSCursorTest extends PHPUnit_Framework_TestCase
{
    /**
     * @var    MongoGridFSFile
     * @access protected
     */
    protected $object;

    /**
     * Sets up the fixture, for example, opens a network connection.
     * This method is called before a test is executed.
     *
     * @access protected
     */
    protected function setUp()
    {
        $m = new Mongo();
        $db = $m->selectDB('phpunit');
        $grid = $db->getGridFS();
        $grid->drop();
        $grid->storeFile('tests/somefile');
        $this->object = $grid->find();
        $this->object->start = memory_get_usage(true);
    }

    protected function tearDown() {
        $this->assertEquals($this->object->start, memory_get_usage(true));
    }


    public function testGetNext() {
        $obj = $this->object->getNext();
        $this->assertTrue($obj instanceof MongoGridFSFile);
    }

    public function testCurrent() {
        $this->assertEquals($this->object->current(), null);
        $this->object->next();
        $obj = $this->object->current();
        $this->assertNotNull($obj);
        $this->assertTrue($obj instanceof MongoGridFSFile);
    }

    public function testKey() {
        foreach ($this->object as $k => $v) {
            $this->assertEquals($k, 'tests/somefile');
        }
    }
}
?>
